// How the window reaches nexus-server, as mosh reaches mosh-server: over ssh it runs `nexus-server new` on the server's
// machine (in the login shell there, so its PATH is the person's), which prints one line, `NEXUS CONNECT <port> <key>`,
// and exits; then the window talks UDP to that port with that key (link/mosh_link.h). The key is printed only on that
// line, read only from it, and never logged, written to a file or put in an error message.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "friends.h"
#include "sync/formats.h"

namespace nexus::link {

struct Connect {
    std::string port;
    std::string key;
    std::uint32_t format = 1;   // the state format the server sends on this link: 1 when the line states none
};

// The line nexus-server new prints. `format` 0 states none: the line a window that stated no format reads (one from
// before formats, which takes nothing after the key).
std::string connectLine(const std::string& port, const std::string& key, std::uint32_t format = 0);
// True for exactly a CONNECT line: "NEXUS CONNECT", a port of 1 to 65535, and a key of 22 base64 characters (mosh's
// printable 128-bit key), then, optionally, "format" and a number from 1, separated by single spaces.
bool parseConnect(const std::string& line, Connect* out);

// The line ssh runs on the server's machine: `exec "$SHELL" -l -c '<command>'`, each word of the command quoted.
std::string remoteLine(const std::vector<std::string>& command);

// The host that names this machine without ssh: the window runs `nexus-server new` itself, as the placed program
// `<home>/.nexus/bin/nexus-server` (`nexus-server setup` puts it there: SETUP.md), and reaches the server on
// 127.0.0.1. So a person with one machine needs no ssh to it (on macOS, Remote Login is an administrator's setting).
inline const char* const kLocalHost = "local";
// Where `nexus-server setup` places the server, under the home directory.
inline const char* const kPlacedServer = ".nexus/bin/nexus-server";
// The address the window reaches a kLocalHost server on.
inline const char* const kLocalAddress = "127.0.0.1";

struct Bootstrap {
    std::string ssh = "/usr/bin/ssh";
    std::string host;                                           // as ssh takes it; kLocalHost: this machine, no ssh
    std::string home;                                           // with kLocalHost: HOME, under which the server is placed
    std::vector<std::string> command = {"nexus-server", "new"}; // run there in the login shell
    int timeoutSeconds = 30;
    // The state format the window reads, stated to nexus-server new in its environment (sync/formats.h); 0 states
    // none, as a window before formats did.
    std::uint32_t stateFormat = sync::kStateFormat;
};

// Whether the bootstrap names this machine (kLocalHost): no ssh, and the server on kLocalAddress.
bool isLocal(const Bootstrap& b);

// The ssh command that runs it: no terminal, never a password prompt (BatchMode: the window has no terminal to ask in).
// The command runs under `env NEXUS_STATE_FORMAT=<n>` when the bootstrap states a format. With kLocalHost it is the
// placed server run directly: `/usr/bin/env [NEXUS_STATE_FORMAT=<n>] <home>/.nexus/bin/nexus-server new`.
Command bootstrapCommand(const Bootstrap& b);

// Runs the command over ssh with `run` and reads its output for the CONNECT line. False, with why (what ssh or the
// command printed on standard error among it, never the key), when ssh fails or prints no CONNECT line.
bool bootstrap(const Bootstrap& b, const CommandRunner& run, Connect* out, std::string* why);

// The numeric address of the host ssh reaches: ssh's own HostName for it (`ssh -G host`, run with `run`), resolved,
// IPv4 first.
bool resolveHost(const Bootstrap& b, const CommandRunner& run, std::string* ip, std::string* why);

} // namespace nexus::link
