// The words the window's port (relay/relay_port.h) starts the relay with, and the text it shows of what the relay's
// process wrote that was not the relay protocol. No OS: built and unit-tested everywhere.

#pragma once

#include <string>
#include <vector>

namespace nexus::relay {

// The relay's program in WSL2 when the window names none: found on the PATH WSL2 gives a process, with ~/.local/bin
// first (where nexus-server is put, docs/README.md). Through a plain `sh -c`, not a login shell: a login shell runs the
// user's profile, and whatever that prints goes to the relay's output ahead of its Hello, which the window refuses.
// "$@" is the relay's arguments, each one word as the window gave it.
extern const char* const kDefaultRelayScript;

// The words wsl.exe runs (after `-e`): the relay and its arguments, for `host` ("localhost": the server in WSL2,
// --local; any other: --server HOST, ssh from WSL2). `relay` is the relay's command in WSL2, its first word the program
// (empty: the default, kDefaultRelayScript) and the rest put after the host (a `--` and a COMMAND for nexus-server new,
// as nexus-link-relay takes them). An empty `relay` is the default with nothing after the host.
std::vector<std::string> relayCommand(const std::string& host, const std::vector<std::string>& relay);

// A Windows command line for `words`, each quoted as CommandLineToArgvW reads it back.
std::string windowsCommandLine(const std::vector<std::string>& words);

// Bytes a process wrote, as one line of plain text to show: UTF-16LE (wsl.exe writes its own messages so, on the
// relay's output and error) decoded, control bytes and runs of spaces as one space, trimmed, at most 300 bytes.
std::string plainText(const std::string& bytes);

} // namespace nexus::relay
