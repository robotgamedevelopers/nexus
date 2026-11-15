// What a terminal tab's shell is: the program, its arguments and where it starts, chosen for the platform. Pure logic:
// no OS, no process, no time, so that the choice is unit-tested on every platform (tests/window/test_terminal_launch.cpp)
// and the one place that starts shells (terminals.cpp, on its start thread) only runs what this says.
//
// On macOS and Linux the program is left empty, so Prax's terminal runs the person's login shell (SHELL, or /bin/sh,
// with -l: pr/terminal.h PrTerminalDesc) in the directory asked for, or the home directory.
//
// On Windows only the overlay window is native and everything else is WSL2 (AGENTS.md, platform targets): a terminal tab
// is a WSL2 shell, never cmd.exe or PowerShell. Prax's terminal would run COMSPEC or cmd.exe for an empty program
// (lib/studio/terminal/src/terminal.cpp, prTerminalCreate), so the program is named: the default WSL2 distribution's
// interactive login bash, with TERM=xterm-256color, in the WSL2 home directory, and the distribution's own .profile and
// .bashrc set the prompt, as on the fleet's Linux machines:
//
//     wsl.exe --cd ~ --exec /usr/bin/env TERM=xterm-256color /bin/bash --login -i
//
// --exec runs bash directly rather than through a shell's -c; env sets TERM inside Linux, because wsl.exe passes a
// Windows variable (Prax's TERM among them) into Linux only when WSLENV names it. A saved directory that is a Linux
// path takes the place of ~, once a probe has found that it is still a directory there.
//
// Before each shell the start thread probes WSL2 (wsl2Probe): it runs `wsl.exe --exec /bin/true`, or `wsl.exe --exec
// /usr/bin/test -d <dir>` for a saved directory, which also starts WSL2's machine if it is stopped. wsl2Verdict reads
// what that gave: WSL2 runs; the saved directory is gone (start in the home directory); or WSL2 is missing, and the tab
// shows one plain line saying what is missing and what to do, with no fallback to another shell.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "friends.h"

namespace nexus {

// Where a terminal tab's shell runs.
enum class ShellHost {
    Posix,  // macOS and Linux: the login shell, on this machine
    Wsl2,   // Windows: a shell in the default WSL2 distribution
};

// This build's: Wsl2 on Windows, Posix elsewhere.
#if defined(_WIN32)
constexpr ShellHost kShellHost = ShellHost::Wsl2;
#else
constexpr ShellHost kShellHost = ShellHost::Posix;
#endif

// The program WSL2 is reached through: the system's wsl.exe, found on the search path (System32).
constexpr const char* kWslProgram = "wsl.exe";
// How long the start thread waits for the probe, in seconds: a stopped WSL2 machine starts within it.
constexpr int kWsl2ProbeTimeoutSeconds = 60;

// A shell to start: what Prax's terminal is given (pr/terminal.h PrTerminalDesc).
struct TerminalLaunch {
    std::string program;                // empty: Prax's terminal runs the login shell (POSIX)
    std::vector<std::string> arguments;
    // The directory the process starts in on this machine: empty for the home directory. For Wsl2 it is always empty
    // (wsl.exe's own --cd says where the shell starts).
    std::string directory;
};

// A Linux absolute path: what a WSL2 shell's saved directory must be to be used.
bool isLinuxPath(std::string_view dir);

// The shell of a terminal tab on `host`. `program` is the one the window was told to run (PraxTerminals::program, set
// by tests), empty for the platform's shell: on Posix it runs as is; on Wsl2 an empty one is the WSL2 bash above, and a
// named one runs as is (a test's own program). `dir` is the directory asked for, empty for the home directory: on Posix
// it is the process's directory (the caller checks it is still a directory); on Wsl2 it is the shell's --cd when it is a
// Linux path, and ~ otherwise (a Windows path, or none).
TerminalLaunch terminalLaunch(ShellHost host, const std::string& program, const std::string& dir);

// The command that probes WSL2 before a shell starts in `dir` (above).
Command wsl2Probe(const std::string& dir);

// What the probe found.
struct Wsl2Verdict {
    // Empty if a shell can start. Otherwise the one plain line the tab shows: what is missing and what to do.
    std::string problem;
    // The saved directory is no longer a directory inside WSL2: the shell starts in the home directory instead.
    bool home = false;
};

// Reads what the probe `probe` (wsl2Probe of `dir`) gave, `result`, with `timeoutSeconds` as it was run.
Wsl2Verdict wsl2Verdict(const Command& probe, const CommandResult& result, const std::string& dir, int timeoutSeconds);

// wsl.exe's own messages as one line of text: wsl.exe writes UTF-16LE to a pipe (unless WSL_UTF8 is set in its
// environment), which is read as UTF-16LE when it starts with a byte order mark or a NUL second byte, and as UTF-8
// otherwise; carriage returns and blank lines go. The first line is kept, with the line that names the error code
// (Wsl/... or WSL_E_...) after it in parentheses; the rest (a link to WSL's issues) is left out.
std::string wslMessage(std::string_view bytes);

} // namespace nexus
