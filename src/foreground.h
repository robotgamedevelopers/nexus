// Foreground process information for terminal tab labels.

#pragma once

#include <string>
#include <vector>

namespace nexus {

// The arguments (argv[0] first) of the leader of the foreground process group on the terminal of `shell`: the shell
// itself at its prompt, or the job it runs. Empty if the shell has exited or the system cannot tell.
std::vector<std::string> foregroundArguments(int shell);

// The two halves of foregroundArguments: the foreground process group on the terminal of `shell`, by its leader's
// process id (-1 if the shell has exited or the system cannot tell), one small system call; and the arguments of the
// process `leader`, a copy of up to the system's argument limit.
int foregroundGroup(int shell);
std::vector<std::string> processArguments(int leader);

} // namespace nexus
