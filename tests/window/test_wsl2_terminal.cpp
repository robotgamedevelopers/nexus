// A Windows terminal tab's shell, for real (src/terminal_launch.h): the probe through wsl.exe, then the launch line on a
// Windows pseudoconsole through Prax's lib/process, as Prax's terminal starts it (pr/terminal.h passes the program,
// arguments and directory to prProcessLaunch with PrProcessStreams_Terminal). It checks that the default WSL2
// distribution's bash starts in the WSL2 home with TERM=xterm-256color as an interactive login shell, prints the prompt
// its own .bashrc sets (whatever that is: the shell's own expansion of PS1, ${PS1@P}, is what stands before each command),
// takes Escape (readline's Escape-period yanks the last argument), and that exit ends it. A functional test: real
// processes, WSL2 and real time. Windows only, run as a user who has a WSL2 distribution (tests/window/CMakeLists.txt).
//
// It cannot drive nexus's own terminals (src/terminals.cpp) on Windows: Prax's terminal needs Prax's lib/app, whose Win32
// backend does not compile at the pin (CMakeLists.txt NEXUS_WINDOW). The launch line and the probe are the same code.

#include <chrono>
#include <cstdio>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "pr/process.h"

#include "terminal_launch.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

PrStringView view(const std::string& s) {
    return PrStringView{s.data(), s.size()};
}

std::vector<PrStringView> views(const std::vector<std::string>& strings) {
    std::vector<PrStringView> out;
    for (const std::string& s : strings) {
        out.push_back(view(s));
    }
    return out;
}

void drain(PrProcess* process, PrProcessStream stream, std::string& into) {
    uint8_t buffer[4096];
    for (;;) {
        const size_t n = prProcessRead(process, stream, buffer, sizeof buffer);
        if (n == 0) {
            return;
        }
        into.append(reinterpret_cast<const char*>(buffer), n);
    }
}

// runCommand's work (src/ais.cpp), over pipes: nexus's own runner is in the window's logic, which Windows does not
// build at the pin (CMakeLists.txt NEXUS_WINDOW_LOGIC).
CommandResult run(PrProcessLauncher* launcher, const Command& command, int timeoutSeconds) {
    CommandResult result;
    const std::vector<PrStringView> arguments = views(command.arguments);
    PrProcessDesc desc{};
    desc.program = view(command.program);
    desc.arguments = arguments.data();
    desc.argumentCount = uint32_t(arguments.size());
    desc.streams = PrProcessStreams_Pipes;
    desc.errors = PrProcessErrors_Pipe;
    PrProcess* process = nullptr;
    const PrStatus launched = prProcessLaunch(launcher, &desc, &process);
    if (launched != PrStatus_Ok || process == nullptr) {
        result.end = launched == PrStatus_NotFound ? CommandResult::End::NotFound : CommandResult::End::NotStarted;
        return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    PrProcessState info{};
    for (;;) {
        drain(process, PrProcessStream_Output, result.out);
        drain(process, PrProcessStream_Error, result.err);
        info = prProcessState(process);
        if (info.exited && info.outputEnded) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            prProcessDestroy(process);
            result.end = CommandResult::End::TimedOut;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    drain(process, PrProcessStream_Error, result.err);
    prProcessDestroy(process);
    result.end = info.signaled ? CommandResult::End::Signaled : CommandResult::End::Exited;
    result.code = info.code;
    return result;
}

// The text of terminal output without its control sequences: CSI (ESC [ ... final), OSC (ESC ] ... BEL or ST), other
// two-byte escapes, and the C0 controls but line feed.
std::string plain(const std::string& raw) {
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == 0x1B && i + 1 < raw.size()) {
            const char kind = raw[i + 1];
            i += 2;
            if (kind == '[') {
                while (i < raw.size() && !(raw[i] >= 0x40 && raw[i] <= 0x7E)) {
                    ++i;
                }
            } else if (kind == ']') {
                while (i < raw.size() && raw[i] != '\a' && !(raw[i] == 0x1B && i + 1 < raw.size() && raw[i + 1] == '\\')) {
                    ++i;
                }
                if (i < raw.size() && raw[i] == 0x1B) {
                    ++i;
                }
            }
            continue;
        }
        if (c < 0x20 && c != '\n') {
            continue;
        }
        out += char(c);
    }
    return out;
}

// A shell on a pseudoconsole, its output gathered as it arrives.
struct Shell {
    PrProcess* process = nullptr;
    std::string raw;

    void read() {
        if (process != nullptr) {
            drain(process, PrProcessStream_Output, raw);
        }
    }

    // Waits up to `seconds` for the output's plain text to match `pattern`; true if it did.
    bool waitFor(const std::regex& pattern, int seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        for (;;) {
            read();
            if (std::regex_search(plain(raw), pattern)) {
                return true;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Waits up to `seconds` for some output and then none for a second; true if it came.
    bool waitQuiet(int seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::size_t seen = 0;
        auto changed = std::chrono::steady_clock::now();
        for (;;) {
            read();
            const auto now = std::chrono::steady_clock::now();
            if (raw.size() != seen) {
                seen = raw.size();
                changed = now;
            } else if (seen != 0 && now - changed >= std::chrono::seconds(1)) {
                return true;
            }
            if (now > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void type(const std::string& text) {
        prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    }
};

// The probe against the real wsl.exe: WSL2 runs, a saved directory that exists is kept, one that is gone starts home,
// and a missing distribution is the one line with wsl.exe's own words (decoded from its UTF-16).
void probeReadsTheRealWsl(PrProcessLauncher* launcher) {
    const int t = kWsl2ProbeTimeoutSeconds;
    Wsl2Verdict fresh = wsl2Verdict(wsl2Probe(""), run(launcher, wsl2Probe(""), t), "", t);
    CHECK(fresh.problem.empty());
    if (!fresh.problem.empty()) {
        std::fprintf(stderr, "probe: %s\n", fresh.problem.c_str());
    }
    Wsl2Verdict kept = wsl2Verdict(wsl2Probe("/tmp"), run(launcher, wsl2Probe("/tmp"), t), "/tmp", t);
    CHECK(kept.problem.empty() && !kept.home);
    const std::string gone = "/no/such/nexus/dir";
    Wsl2Verdict home = wsl2Verdict(wsl2Probe(gone), run(launcher, wsl2Probe(gone), t), gone, t);
    CHECK(home.problem.empty() && home.home);
    const Command missing{kWslProgram, {"--distribution", "nexus-no-such-distribution", "--exec", "/bin/true"},
        "wsl.exe --distribution nexus-no-such-distribution --exec /bin/true"};
    Wsl2Verdict none = wsl2Verdict(missing, run(launcher, missing, t), "", t);
    std::printf("no distribution: %s\n", none.problem.c_str());
    CHECK(none.problem.find("WSL2 has no Linux distribution it can start") == 0);
    CHECK(none.problem.find("WSL_E_DISTRO_NOT_FOUND") != std::string::npos);
    CHECK(none.problem.find('\n') == std::string::npos);
    CHECK(none.problem.find('\0') == std::string::npos);
}

void wsl2ShellStartsWithItsPrompt(PrProcessLauncher* launcher) {
    const TerminalLaunch launch = terminalLaunch(ShellHost::Wsl2, "", "");
    std::string line = launch.program;
    for (const std::string& a : launch.arguments) {
        line += " " + a;
    }
    std::printf("launch: %s\n", line.c_str());
    const std::vector<PrStringView> arguments = views(launch.arguments);
    // The variables Prax's terminal adds on Windows (terminal.cpp kEnvironment), as a terminal tab gets them.
    const PrStringView environment[] = {PrStringView("TERM=xterm-256color"), PrStringView("COLORTERM=truecolor"),
        PrStringView("TERM_PROGRAM=prax")};
    PrProcessDesc desc{};
    desc.program = view(launch.program);
    desc.arguments = arguments.data();
    desc.argumentCount = uint32_t(arguments.size());
    desc.environment = environment;
    desc.environmentCount = 3;
    desc.streams = PrProcessStreams_Terminal;
    desc.terminalExtent = PrUVec2{160, 40};
    Shell shell;
    const PrStatus status = prProcessLaunch(launcher, &desc, &shell.process);
    CHECK(status == PrStatus_Ok && shell.process != nullptr);
    if (shell.process == nullptr) {
        std::fprintf(stderr, "prProcessLaunch on a pseudoconsole: status %d\n", int(status));
        return;
    }
    // The shell is up and waiting once it has printed something and then nothing for a second (its prompt last).
    CHECK(shell.waitQuiet(60));
    // The prompt is the one the distribution's .bashrc sets: the shell's own expansion of PS1 is printed between P= and
    // =P, and that text stands before each command, the first one included (so it is the prompt bash started with).
    shell.type("printf 'P=%s=P\\n' \"${PS1@P}\"\r");
    std::smatch m;
    std::string expanded;
    if (shell.waitFor(std::regex(R"(\nP=(.+)=P)"), 20)) {
        const std::string text = plain(shell.raw);
        std::regex_search(text, m, std::regex(R"(\nP=(.+)=P)"));
        expanded = m[1].str();
        while (!expanded.empty() && expanded.back() == ' ') {
            expanded.pop_back();
        }
    }
    std::printf("prompt: [%s]\n", expanded.c_str());
    CHECK(!expanded.empty());
    if (!expanded.empty()) {
        const std::string text = plain(shell.raw);
        const std::size_t first = text.find(expanded);
        CHECK(first != std::string::npos && first < text.find("printf"));
        if (first == std::string::npos || first >= text.find("printf")) {
            std::fprintf(stderr, "--- the screen so far ---\n%s\n--- raw ---\n", text.c_str());
            for (char c : shell.raw) {
                if (c == 0x1B) {
                    std::fputs("\\e", stderr);
                } else {
                    std::fputc(c, stderr);
                }
            }
            std::fputs("\n---\n", stderr);
        }
    }
    // Never Windows' shells.
    CHECK(plain(shell.raw).find("Microsoft Windows") == std::string::npos);
    CHECK(plain(shell.raw).find("PS C:\\") == std::string::npos);
    // TERM inside Linux, the shell's directory is its home, and it is an interactive login bash.
    shell.type("echo \"T=$TERM,H=$HOME,P=$PWD,L=$(shopt -q login_shell && echo y),I=${-//[^i]/}\"\r");
    CHECK(shell.waitFor(std::regex(R"(T=xterm-256color,H=(/[^,\n]+),P=\1,L=y,I=i)"), 20));
    // Escape reaches the shell: Escape then period is readline's yank-last-arg, which puts back A1B2.
    shell.type("echo A1B2\r");
    CHECK(shell.waitFor(std::regex(R"(\nA1B2)"), 20));
    shell.type("echo X\x1b.Y\r");
    CHECK(shell.waitFor(std::regex(R"(\nXA1B2Y)"), 20));
    // exit ends it, as it closes a tab.
    shell.type("exit\r");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    PrProcessState info = prProcessState(shell.process);
    while (!(info.exited && info.outputEnded) && std::chrono::steady_clock::now() < deadline) {
        shell.read();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        info = prProcessState(shell.process);
    }
    CHECK(info.exited && info.code == 0);
    const std::string text = plain(shell.raw);
    std::printf("--- the shell's screen, as text ---\n%s\n---\n", text.c_str());
    prProcessDestroy(shell.process);
}

} // namespace

int main() {
    PrProcessLauncher* launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    CHECK(launcher != nullptr);
    if (launcher == nullptr) {
        return 1;
    }
    probeReadsTheRealWsl(launcher);
    wsl2ShellStartsWithItsPrompt(launcher);
    prProcessLauncherDestroy(launcher);
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("wsl2_terminal: all passed\n");
    return 0;
}
