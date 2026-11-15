// What a terminal tab's shell is (src/terminal_launch.h): the program, arguments and directory on each platform, the
// probe of WSL2, and what the probe's results mean, from made-up results. Pure logic: no process, no real time, no OS,
// nothing outside the test. It runs on every platform, so the Windows choice is checked on macOS and Linux too.

#include <cstdio>
#include <string>
#include <vector>

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

using Strings = std::vector<std::string>;

const Strings kShell = {"--exec", "/usr/bin/env", "TERM=xterm-256color", "/bin/bash", "--login", "-i"};

Strings wslArguments(const std::string& cd) {
    Strings a = {"--cd", cd};
    a.insert(a.end(), kShell.begin(), kShell.end());
    return a;
}

// wsl.exe's UTF-16LE: each ASCII byte followed by a NUL.
std::string utf16(const std::string& ascii) {
    std::string out;
    for (char c : ascii) {
        out += c;
        out += '\0';
    }
    return out;
}

CommandResult exited(int code, std::string out = {}) {
    CommandResult r;
    r.end = CommandResult::End::Exited;
    r.code = code;
    r.out = std::move(out);
    return r;
}

CommandResult ended(CommandResult::End end) {
    CommandResult r;
    r.end = end;
    return r;
}

// On macOS and Linux nothing changes: an empty program is Prax's login shell, in the directory asked for.
void posixIsTheLoginShell() {
    TerminalLaunch home = terminalLaunch(ShellHost::Posix, "", "");
    CHECK(home.program.empty());
    CHECK(home.arguments.empty());
    CHECK(home.directory.empty());
    TerminalLaunch kept = terminalLaunch(ShellHost::Posix, "", "/Users/someone/work");
    CHECK(kept.program.empty());
    CHECK(kept.directory == "/Users/someone/work");
    TerminalLaunch named = terminalLaunch(ShellHost::Posix, "/bin/sh", "/tmp");
    CHECK(named.program == "/bin/sh");
    CHECK(named.arguments.empty());
    CHECK(named.directory == "/tmp");
}

// On Windows a new tab is the default WSL2 distribution's interactive login bash in the WSL2 home, TERM set inside
// Linux: never cmd.exe, PowerShell or COMSPEC, and never a Windows directory.
void windowsIsAWsl2LoginBashInTheWsl2Home() {
    TerminalLaunch t = terminalLaunch(ShellHost::Wsl2, "", "");
    CHECK(t.program == "wsl.exe");
    CHECK(t.arguments == wslArguments("~"));
    CHECK(t.directory.empty());
    for (const std::string& a : t.arguments) {
        CHECK(a.find("cmd") == std::string::npos);
        CHECK(a.find("powershell") == std::string::npos);
        CHECK(a != "-d" && a != "--distribution");  // the default distribution
    }
}

// A saved directory that is a Linux path is where the restored shell starts; a Windows path (what a shell that never
// reported a Linux directory leaves) or none starts in the WSL2 home.
void windowsDirectoryIsALinuxPathOrHome() {
    CHECK(terminalLaunch(ShellHost::Wsl2, "", "/home/someone/src").arguments == wslArguments("/home/someone/src"));
    CHECK(terminalLaunch(ShellHost::Wsl2, "", "/home/some one/a b").arguments == wslArguments("/home/some one/a b"));
    CHECK(terminalLaunch(ShellHost::Wsl2, "", "C:\\Users\\someone").arguments == wslArguments("~"));
    CHECK(terminalLaunch(ShellHost::Wsl2, "", "\\\\wsl.localhost\\Ubuntu\\home").arguments == wslArguments("~"));
    CHECK(terminalLaunch(ShellHost::Wsl2, "", "relative/dir").arguments == wslArguments("~"));
    CHECK(terminalLaunch(ShellHost::Wsl2, "", "/srv").directory.empty());
    CHECK(isLinuxPath("/"));
    CHECK(!isLinuxPath(""));
    CHECK(!isLinuxPath("C:/Users"));
}

// A program the window was told to run (a test's own) runs as it is on Windows too, with no WSL2 around it.
void windowsNamedProgramRunsAsIs() {
    TerminalLaunch t = terminalLaunch(ShellHost::Wsl2, "C:\\tools\\fake.exe", "/home/x");
    CHECK(t.program == "C:\\tools\\fake.exe");
    CHECK(t.arguments.empty());
    CHECK(t.directory.empty());
}

// The probe: /bin/true for a new tab, test -d for a saved Linux directory, in the default distribution.
void probeCommands() {
    Command fresh = wsl2Probe("");
    CHECK(fresh.program == "wsl.exe");
    CHECK(fresh.arguments == (Strings{"--exec", "/bin/true"}));
    CHECK(fresh.shown == "wsl.exe --exec /bin/true");
    CHECK(wsl2Probe("C:\\Users\\x").arguments == (Strings{"--exec", "/bin/true"}));
    Command saved = wsl2Probe("/home/x/src");
    CHECK(saved.arguments == (Strings{"--exec", "/usr/bin/test", "-d", "/home/x/src"}));
}

void verdictRuns() {
    Wsl2Verdict v = wsl2Verdict(wsl2Probe(""), exited(0), "", 60);
    CHECK(v.problem.empty());
    CHECK(!v.home);
    Wsl2Verdict kept = wsl2Verdict(wsl2Probe("/home/x"), exited(0), "/home/x", 60);
    CHECK(kept.problem.empty());
    CHECK(!kept.home);
}

// test -d's 1: the directory is gone and WSL2 runs, so the shell starts in the home directory, with no problem line.
void verdictGoneDirectoryStartsHome() {
    Wsl2Verdict v = wsl2Verdict(wsl2Probe("/home/x/gone"), exited(1), "/home/x/gone", 60);
    CHECK(v.problem.empty());
    CHECK(v.home);
    // With no saved directory, 1 is not test's answer: it is WSL2's failure.
    Wsl2Verdict fresh = wsl2Verdict(wsl2Probe(""), exited(1), "", 60);
    CHECK(!fresh.problem.empty());
    CHECK(!fresh.home);
}

// No wsl.exe at all: WSL2 is not installed, said in one line with the fix.
void verdictNoWslExe() {
    for (CommandResult::End end : {CommandResult::End::NotFound, CommandResult::End::NotStarted}) {
        Wsl2Verdict v = wsl2Verdict(wsl2Probe(""), ended(end), "", 60);
        CHECK(v.problem.find("WSL2 is not installed") == 0);
        CHECK(v.problem.find("wsl.exe was not found") != std::string::npos);
        CHECK(v.problem.find("wsl --install") != std::string::npos);
        CHECK(v.problem.find('\n') == std::string::npos);
        CHECK(!v.home);
    }
}

// No distribution (wsl.exe's -1, as Windows gives it, with its UTF-16 message): one line with wsl's own words and the fix.
void verdictNoDistribution() {
    const std::string said = utf16("There is no distribution with the supplied name.\r\nError code: "
        "Wsl/Service/WSL_E_DISTRO_NOT_FOUND\r\nIf this error was unexpected, please consider searching for existing "
        "issues or filing a new issue at https://github.com/microsoft/WSL/issues.\r\n");
    Wsl2Verdict v = wsl2Verdict(wsl2Probe(""), exited(-1, said), "", 60);
    CHECK(v.problem == "WSL2 has no Linux distribution it can start: `wsl.exe --exec /bin/true` exited with status -1: "
        "There is no distribution with the supplied name. (Error code: Wsl/Service/WSL_E_DISTRO_NOT_FOUND). Install one "
        "with `wsl --install -d Ubuntu` in PowerShell (or run `wsl --status` to see what is wrong), then open a new "
        "terminal tab");
    CHECK(!v.home);
    // A saved directory does not change it: -1 is never test's answer.
    CHECK(!wsl2Verdict(wsl2Probe("/home/x"), exited(-1, said), "/home/x", 60).problem.empty());
    // Silent: said so.
    Wsl2Verdict quiet = wsl2Verdict(wsl2Probe(""), exited(4), "", 60);
    CHECK(quiet.problem.find("exited with status 4 and printed nothing.") != std::string::npos);
    // The words on standard error when standard output has none.
    CommandResult onErr = exited(-1);
    onErr.err = "The Windows Subsystem for Linux is not installed.\n";
    CHECK(wsl2Verdict(wsl2Probe(""), onErr, "", 60).problem.find(": The Windows Subsystem for Linux is not installed.")
        != std::string::npos);
}

void verdictTimedOutAndSignaled() {
    Wsl2Verdict v = wsl2Verdict(wsl2Probe(""), ended(CommandResult::End::TimedOut), "", 60);
    CHECK(v.problem == "WSL2 did not answer: `wsl.exe --exec /bin/true` did not finish within 60 seconds. Run `wsl "
        "--status` in PowerShell to see why, then open a new terminal tab");
    CommandResult signaled = ended(CommandResult::End::Signaled);
    signaled.code = 9;
    CHECK(wsl2Verdict(wsl2Probe(""), signaled, "", 60).problem.find("was ended by signal 9") != std::string::npos);
}

// wsl.exe's messages: UTF-16LE with or without a byte order mark, non-ASCII text, UTF-8, and nothing.
void messages() {
    CHECK(wslMessage("").empty());
    CHECK(wslMessage(utf16("\r\n  \r\n")).empty());
    CHECK(wslMessage(utf16("one line\r\n")) == "one line");
    CHECK(wslMessage(std::string("\xFF\xFE", 2) + utf16("bom first\r\n")) == "bom first");
    CHECK(wslMessage("plain utf-8\nsecond\n") == "plain utf-8");
    CHECK(wslMessage("\xEF\xBB\xBFutf-8 bom\n") == "utf-8 bom");
    CHECK(wslMessage("first\nError code: Wsl/E_X\nmore\n") == "first (Error code: Wsl/E_X)");
    // "é" (U+00E9) and "€" (U+20AC) and an astral "😀" (U+1F600, a surrogate pair) in UTF-16LE.
    const std::string accented = std::string("\xE9\x00 \x00\xAC\x20 \x00\x3D\xD8\x00\xDE", 12);
    CHECK(wslMessage(accented) == "\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
    // An unpaired surrogate reads as U+FFFD.
    CHECK(wslMessage(std::string("a\x00\x00\xD8", 4)) == "a\xEF\xBF\xBD");
}

} // namespace

int main() {
    posixIsTheLoginShell();
    windowsIsAWsl2LoginBashInTheWsl2Home();
    windowsDirectoryIsALinuxPathOrHome();
    windowsNamedProgramRunsAsIs();
    probeCommands();
    verdictRuns();
    verdictGoneDirectoryStartsHome();
    verdictNoWslExe();
    verdictNoDistribution();
    verdictTimedOutAndSignaled();
    messages();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("terminal_launch_unit: all passed\n");
    return 0;
}
