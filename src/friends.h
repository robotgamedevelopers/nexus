// The friends to show as tabs, read from `nova-config friend list --json`.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "model.h"

namespace nexus {

// The enabled friends, in the order the command lists them, or why there are none.
struct FriendList {
    std::vector<FriendInfo> friends;
    std::string error;          // empty when `friends` came from the command; otherwise one line for the window
};

// Reads the command's JSON: {"result": {"status": "ok"}, "items": [{"fields": {"name", "model", "enabled"}}]}. Keeps
// the friends whose `enabled` is true. Pure: no OS.
FriendList parseFriendList(std::string_view json);

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

// How long loadFriends waits for the command. It runs before the window opens.
constexpr int kFriendListTimeoutSeconds = 5;

// nova-config with `arguments`. With `override` (the client config's `nova-config`, a full path to nova-config;
// client_home.h), that program is run
// directly. Otherwise nova-config is run through `shell` (SHELL, or /bin/sh if unset) as a login shell, so that it is
// found on the PATH the person's terminal has: an app launched from Finder or at login gets a minimal PATH. On Windows,
// which has no login shell, nova-config is run directly from PATH and `shell` is not used.
Command novaConfigCommand(const char* override, const char* shell, const std::vector<std::string>& arguments);

// The command that lists the friends: novaConfigCommand with `friend list --json`.
Command friendListCommand(const char* override, const char* shell);

// What went wrong when `command` gave `r`, naming the command, what happened and what to do; empty if it exited 0.
std::string commandProblem(const Command& command, const CommandResult& r, int timeoutSeconds);

// Runs `command` with `run` and parses what it prints. Every error names the command, what happened and what to do.
FriendList loadFriends(const Command& command, const CommandRunner& run);

// Runs a command as a child process (Prax's lib/process), with this process's environment.
CommandResult runCommand(const Command& command, int timeoutSeconds);

// loadFriends with friendListCommand(the client config's nova-config, SHELL) and runCommand.
FriendList loadFriends();

} // namespace nexus
