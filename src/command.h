// Running a command from the window's logic, with a deadline, behind a seam tests fill with a fake: the fleet's
// machine list (remote.h), the sprint board's address (sprint.h) and the link's ssh bootstrap (link/bootstrap.h) run
// through it.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace nexus {

// The program to run and its arguments, and the command line as a person would type it, for messages.
struct Command {
    std::string program;
    std::vector<std::string> arguments;
    std::string shown;
};

// What running a command gave: how it ended, its status or signal, and what it printed.
struct CommandResult {
    enum class End { Exited, Signaled, NotFound, NotStarted, TimedOut };
    End end = End::NotStarted;
    int code = 0;               // the exit status, or the signal
    std::string out;
    std::string err;
};

// Runs a command, giving up after `timeoutSeconds`. The seam that tests fill with a fake: no test spawns a process.
using CommandRunner = std::function<CommandResult(const Command& command, int timeoutSeconds)>;

// nova-config with `arguments`. With `override` (the client config's `nova-config`, a full path to nova-config;
// client_home.h), that program is run directly. Otherwise nova-config is run through `shell` (SHELL, or /bin/sh if
// unset) as a login shell, so that it is found on the PATH the person's terminal has: an app launched from Finder or at
// login gets a minimal PATH. On Windows, which has no login shell, nova-config is run directly from PATH and `shell` is
// not used. The fleet's machine list (remote.h) and the sprint board's address (sprint.h) are read through it; the
// friends never are: they are the server's.
Command novaConfigCommand(const char* override, const char* shell, const std::vector<std::string>& arguments);

// What went wrong when `command` (a nova-config command) gave `r`, naming the command, what happened and what to do;
// empty if it exited 0.
std::string commandProblem(const Command& command, const CommandResult& r, int timeoutSeconds);

// Runs a command as a child process (Prax's lib/process), with this process's environment.
CommandResult runCommand(const Command& command, int timeoutSeconds);

} // namespace nexus
