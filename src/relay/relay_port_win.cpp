#include "relay/relay_port.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace nexus::relay {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string lastError(const char* what) {
    const DWORD code = GetLastError();
    char text[512] = {};
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text,
        sizeof text, nullptr);
    std::string message(text, n);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.'))
        message.pop_back();
    return std::string(what) + ": " + message + " (error " + std::to_string(code) + ")";
}

// wsl.exe from System32, not whatever the PATH finds first.
std::string systemWsl() {
    char dir[MAX_PATH];
    const UINT n = GetSystemDirectoryA(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "wsl.exe";
    return std::string(dir, n) + "\\wsl.exe";
}

constexpr DWORD kStopWaitMs = 3000;
// Of what the relay's process writes before its Hello, kept to show.
constexpr std::size_t kBeforeHelloMax = 4096;

// RelayLife's clock: milliseconds of the steady clock.
LifeMs nowMs() {
    return static_cast<LifeMs>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

// One relay process and its pipes' ends on this side.
struct RelayPort::Process {
    PROCESS_INFORMATION info{};
    HANDLE input = nullptr;     // the relay's standard input, written here
    HANDLE output = nullptr;    // its standard output, read here
    HANDLE errors = nullptr;    // its standard error, read here

    ~Process() {
        for (HANDLE h : {input, output, errors, info.hProcess, info.hThread})
            if (h != nullptr) CloseHandle(h);
    }

    bool start(const std::string& wsl, const std::vector<std::string>& words, std::string* why) {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE childIn = nullptr, childOut = nullptr, childErr = nullptr;
        if (!CreatePipe(&childIn, &input, &inherit, 0) || !CreatePipe(&output, &childOut, &inherit, 0) ||
                !CreatePipe(&errors, &childErr, &inherit, 0)) {
            *why = lastError("cannot make the relay's pipes");
            for (HANDLE h : {childIn, childOut, childErr})
                if (h != nullptr) CloseHandle(h);
            return false;
        }
        // Only the child's ends are inherited.
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errors, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = childIn;
        si.hStdOutput = childOut;
        si.hStdError = childErr;
        std::vector<std::string> all = {wsl, "-e"};
        all.insert(all.end(), words.begin(), words.end());
        std::wstring line = wide(windowsCommandLine(all));
        const std::wstring program = wide(wsl);
        const BOOL made = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &info);
        const std::string failed = made ? std::string() : lastError(("cannot start " + wsl).c_str());
        CloseHandle(childIn);
        CloseHandle(childOut);
        CloseHandle(childErr);
        if (!made) {
            *why = failed;
            return false;
        }
        return true;
    }
};

RelayPort::RelayPort(Options options) : options_(std::move(options)) {
    if (options_.wsl == "wsl.exe") options_.wsl = systemWsl();
    thread_ = std::thread([this] { run(); });
}

RelayPort::~RelayPort() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        life_.stop();
    }
    changed_.notify_all();
    thread_.join();
}

bool RelayPort::poll(LinkState* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    return life_.poll(out);
}

void RelayPort::requestHistory(sync::SessionId id, std::uint64_t epoch, std::uint64_t before) {
    std::lock_guard<std::mutex> lock(mutex_);
    life_.history(id, epoch, before);
    changed_.notify_all();
}

std::uint64_t RelayPort::send(sync::Action action) {
    std::uint64_t number = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        number = life_.send(action);
        changed_.notify_all();
    }
    return number;
}

void RelayPort::run() {
    const std::vector<std::string> words = relayCommand(options_.host, options_.relay);
    auto wake = [this] {
        if (options_.wake) options_.wake();
    };
    for (;;) {
        {
            // The backoff RelayLife decides, or the window's exit.
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_ && !life_.mayStart(nowMs()))
                changed_.wait_for(lock, std::chrono::milliseconds(life_.waitMs(nowMs())));
            if (stopping_) return;
        }
        Process process;
        std::string why;
        const bool running = process.start(options_.wsl, words, &why);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            life_.started(nowMs());
        }
        if (running) {
            bool writerDone = false;
            // The writer: what RelayLife hands it, in order, its Hello first; at stop, the input closes, which ends the
            // relay.
            std::thread writer([&] {
                for (;;) {
                    std::string bytes;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        changed_.wait(lock, [&] { return stopping_ || writerDone || life_.wantsWrite(); });
                        if (stopping_ || writerDone || !life_.take(&bytes)) break;
                    }
                    std::size_t done = 0;
                    bool ok = true;
                    while (ok && done < bytes.size()) {
                        DWORD n = 0;
                        ok = WriteFile(process.input, bytes.data() + done, static_cast<DWORD>(bytes.size() - done),
                            &n, nullptr) && n > 0;
                        done += n;
                    }
                    wipe(&bytes);
                    if (!ok) break;
                }
                CloseHandle(process.input);
                process.input = nullptr;
            });
            // Its standard error, line by line, into the window's log.
            std::string lastLine;
            std::thread errors([&] {
                std::string pending;
                char buf[4096];
                DWORD n = 0;
                while (ReadFile(process.errors, buf, sizeof buf, &n, nullptr) && n > 0) {
                    pending.append(buf, n);
                    std::size_t end;
                    while ((end = pending.find('\n')) != std::string::npos) {
                        std::string line = plainText(pending.substr(0, end));
                        pending.erase(0, end + 1);
                        if (line.empty()) continue;
                        std::fprintf(stderr, "nexus: link in WSL2: %s\n", line.c_str());
                        std::lock_guard<std::mutex> lock(mutex_);
                        lastLine = std::move(line);
                    }
                }
            });
            // A stop that the relay does not answer within kStopWaitMs ends it.
            std::thread stopper([&] {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || writerDone; });
                if (writerDone) return;
                lock.unlock();
                if (WaitForSingleObject(process.info.hProcess, kStopWaitMs) == WAIT_TIMEOUT)
                    TerminateProcess(process.info.hProcess, 1);
            });
            // Its standard output, here: the relay's frames.
            char buf[65536];
            DWORD n = 0;
            std::string broken;
            // What its process wrote before the relay's Hello was read: wsl.exe's own messages, or a shell's, shown
            // as text when the relay ends without its Hello or with bytes that are not the protocol.
            std::string beforeHello;
            bool helloRead = false;
            while (ReadFile(process.output, buf, sizeof buf, &n, nullptr) && n > 0) {
                bool fed;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!helloRead && beforeHello.size() < kBeforeHelloMax)
                        beforeHello.append(buf, std::min<std::size_t>(n, kBeforeHelloMax - beforeHello.size()));
                    fed = life_.feed(buf, n, &broken);
                    helloRead = life_.phase() == RelayLife::Phase::Running;
                }
                if (!fed) {
                    TerminateProcess(process.info.hProcess, 1);
                    break;
                }
                wake();
            }
            // The relay has ended only once its process has: RelayLife hears of it after, never before.
            WaitForSingleObject(process.info.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(process.info.hProcess, &code);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                writerDone = true;
            }
            changed_.notify_all();
            writer.join();
            stopper.join();
            errors.join();
            std::lock_guard<std::mutex> lock(mutex_);
            // Every reason there is, none hiding another: the protocol's refusal, the exit code, the last line of its
            // standard error, and what it wrote before its Hello.
            why = broken.empty() ? "nexus-link-relay in WSL2 ended" : "nexus-link-relay in WSL2 was ended: " + broken;
            why += " (exit " + std::to_string(code) + ")";
            if (!lastLine.empty()) why += ": " + lastLine;
            if (!helloRead) {
                const std::string text = plainText(beforeHello);
                if (!text.empty()) why += "; before its hello its output was \"" + text + "\"";
            }
            if (code == 127)
                why += "; install nexus in WSL2 so that ~/.local/bin holds nexus-link-relay";
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            life_.ended(why, nowMs());
            if (stopping_) return;
            std::fprintf(stderr, "nexus: %s\n", why.c_str());
        }
        wake();
    }
}

} // namespace nexus::relay
