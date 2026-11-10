// How the window reaches nexus-server, as mosh reaches mosh-server: over ssh it runs `nexus-server new` on the server's
// machine (in the login shell there, so its PATH is the person's), which prints one line, `NEXUS CONNECT <port> <key>`,
// and exits; then the window talks UDP to that port with that key (link/mosh_link.h). The key is printed only on that
// line, read only from it, and never logged, written to a file or put in an error message.

#pragma once

#include <string>
#include <vector>

#include "friends.h"

namespace nexus::link {

struct Connect {
    std::string port;
    std::string key;
};

// The line nexus-server new prints.
std::string connectLine(const std::string& port, const std::string& key);
// True for exactly a CONNECT line: "NEXUS CONNECT", a port of 1 to 65535, and a key of 22 base64 characters (mosh's
// printable 128-bit key), separated by single spaces.
bool parseConnect(const std::string& line, Connect* out);

// The line ssh runs on the server's machine: `exec "$SHELL" -l -c '<command>'`, each word of the command quoted.
std::string remoteLine(const std::vector<std::string>& command);

struct Bootstrap {
    std::string ssh = "/usr/bin/ssh";
    std::string host;                                           // as ssh takes it
    std::vector<std::string> command = {"nexus-server", "new"}; // run there in the login shell
    int timeoutSeconds = 30;
};

// The ssh command that runs it: no terminal, never a password prompt (BatchMode: the window has no terminal to ask in).
Command bootstrapCommand(const Bootstrap& b);

// Runs the command over ssh with `run` and reads its output for the CONNECT line. False, with why (what ssh or the
// command printed on standard error among it, never the key), when ssh fails or prints no CONNECT line.
bool bootstrap(const Bootstrap& b, const CommandRunner& run, Connect* out, std::string* why);

// The numeric address of the host ssh reaches: ssh's own HostName for it (`ssh -G host`, run with `run`), resolved,
// IPv4 first.
bool resolveHost(const Bootstrap& b, const CommandRunner& run, std::string* ip, std::string* why);

} // namespace nexus::link
