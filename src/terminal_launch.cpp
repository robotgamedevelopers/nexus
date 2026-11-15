#include "terminal_launch.h"

#include <cstddef>
#include <cstdint>

namespace nexus {

namespace {

// The shell of a WSL2 terminal tab, after `wsl.exe --cd <dir>` (terminal_launch.h).
const char* const kWsl2Shell[] = {"--exec", "/usr/bin/env", "TERM=xterm-256color", "/bin/bash", "--login", "-i"};

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// UTF-16LE bytes as UTF-8, an unpaired surrogate as U+FFFD; a trailing odd byte is dropped.
std::string fromUtf16(std::string_view bytes) {
    std::string out;
    std::size_t i = 0;
    auto unit = [&](std::size_t at) {
        return uint32_t(uint8_t(bytes[at])) | (uint32_t(uint8_t(bytes[at + 1])) << 8);
    };
    while (i + 1 < bytes.size()) {
        uint32_t c = unit(i);
        i += 2;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < bytes.size()) {
            const uint32_t low = unit(i);
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        appendUtf8(out, c);
    }
    return out;
}

// True if `bytes` read as UTF-16LE: a byte order mark, or a NUL as the second byte (wsl.exe's messages begin with ASCII).
bool looksUtf16(std::string_view bytes) {
    if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xFF && uint8_t(bytes[1]) == 0xFE) {
        return true;
    }
    return bytes.size() >= 2 && bytes[0] != 0 && bytes[1] == 0;
}

std::string trimmed(std::string_view s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        --end;
    }
    return std::string(s.substr(begin, end - begin));
}

} // namespace

bool isLinuxPath(std::string_view dir) {
    return !dir.empty() && dir.front() == '/';
}

TerminalLaunch terminalLaunch(ShellHost host, const std::string& program, const std::string& dir) {
    TerminalLaunch launch;
    if (host == ShellHost::Posix || !program.empty()) {
        launch.program = program;
        launch.directory = host == ShellHost::Posix ? dir : std::string();
        return launch;
    }
    launch.program = kWslProgram;
    launch.arguments = {"--cd", isLinuxPath(dir) ? dir : std::string("~")};
    for (const char* a : kWsl2Shell) {
        launch.arguments.emplace_back(a);
    }
    return launch;
}

Command wsl2Probe(const std::string& dir) {
    std::vector<std::string> arguments = isLinuxPath(dir)
        ? std::vector<std::string>{"--exec", "/usr/bin/test", "-d", dir}
        : std::vector<std::string>{"--exec", "/bin/true"};
    std::string shown = kWslProgram;
    for (const std::string& a : arguments) {
        shown += " " + a;
    }
    return Command{kWslProgram, arguments, shown};
}

std::string wslMessage(std::string_view bytes) {
    std::string text = looksUtf16(bytes) ? fromUtf16(bytes) : std::string(bytes);
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        text.erase(0, 3);
    }
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line;
        for (std::size_t i = at; i < end; ++i) {
            if (text[i] != '\r' && text[i] != '\0') {
                line += text[i];
            }
        }
        line = trimmed(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
        at = end + 1;
    }
    if (lines.empty()) {
        return {};
    }
    std::string message = lines.front();
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].find("Wsl/") != std::string::npos || lines[i].find("WSL_E_") != std::string::npos) {
            message += " (" + lines[i] + ")";
            break;
        }
    }
    return message;
}

Wsl2Verdict wsl2Verdict(const Command& probe, const CommandResult& result, const std::string& dir,
        int timeoutSeconds) {
    Wsl2Verdict verdict;
    const std::string again = "then open a new terminal tab";
    switch (result.end) {
        case CommandResult::End::NotFound:
        case CommandResult::End::NotStarted:
            verdict.problem = "WSL2 is not installed: " + probe.program + " was not found. Install it with `wsl "
                "--install` in PowerShell and restart Windows, " + again;
            return verdict;
        case CommandResult::End::TimedOut:
            verdict.problem = "WSL2 did not answer: `" + probe.shown + "` did not finish within " +
                std::to_string(timeoutSeconds) + " seconds. Run `wsl --status` in PowerShell to see why, " + again;
            return verdict;
        case CommandResult::End::Signaled:
        case CommandResult::End::Exited:
            break;
    }
    if (result.end == CommandResult::End::Exited && result.code == 0) {
        return verdict;
    }
    // test -d's own "no": the saved directory is gone, and WSL2 runs.
    if (result.end == CommandResult::End::Exited && result.code == 1 && isLinuxPath(dir)) {
        verdict.home = true;
        return verdict;
    }
    std::string said = wslMessage(result.out);
    if (said.empty()) {
        said = wslMessage(result.err);
    }
    verdict.problem = "WSL2 has no Linux distribution it can start: `" + probe.shown + "` " +
        (result.end == CommandResult::End::Signaled ? "was ended by signal " : "exited with status ") +
        std::to_string(result.code) + (said.empty() ? " and printed nothing" : ": " + said) +
        ". Install one with `wsl --install -d Ubuntu` in PowerShell (or run `wsl --status` to see what is wrong), " +
        again;
    return verdict;
}

} // namespace nexus
