// What a terminal's shell runs in the foreground, read from the system (macOS: libproc and sysctl). A workaround: Prax's
// terminal names neither its program's process id nor its foreground process (pr/terminal.h has the foreground's
// directory only, prTerminalWorkingDirectory), so nexus finds the shell as the child process that prTerminalCreate
// started, and its terminal's foreground process group from the shell. A request for Prax: the foreground process's id
// and arguments of a PrTerminal.

#pragma once

#include <string>
#include <vector>

namespace nexus {

// This process's children, by process id. Empty where the system cannot list them.
std::vector<int> childProcesses();

// Of the processes in `after` and not in `before`, the one that leads a session on a terminal: the shell a terminal
// started between the two lists. -1 if there is not exactly one.
int startedShell(const std::vector<int>& before, const std::vector<int>& after);

// The arguments (argv[0] first) of the leader of the foreground process group on the terminal of `shell`: the shell
// itself at its prompt, or the job it runs. Empty if the shell has exited or the system cannot tell.
std::vector<std::string> foregroundArguments(int shell);

// The two halves of foregroundArguments: the foreground process group on the terminal of `shell`, by its leader's
// process id (-1 if the shell has exited or the system cannot tell), one small system call; and the arguments of the
// process `leader`, a copy of up to the system's argument limit.
int foregroundGroup(int shell);
std::vector<std::string> processArguments(int leader);

} // namespace nexus
