// How the window reaches nexus-server, as mosh reaches mosh-server: over ssh it runs `nexus-server new` on the server's
// machine (in the login shell there, so its PATH is the person's), which prints one line, `NEXUS CONNECT <port> <key>`,
// and exits; then the window talks UDP to that port with that key (link/mosh_link.h). The key is printed only on that
// line, read only from it, and never logged, written to a file or put in an error message.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "command.h"
#include "sync/formats.h"

namespace nexus::link {

struct Connect {
    std::string port;
    std::string key;
    std::uint32_t format = 1;   // the state format the server sends on this link: 1 when the line states none
    // The server's Hello (sync/formats.h): from the line's `hello` when it states one, else that of a server from
    // before Hello, which speaks `format` and works with every older one.
    sync::Hello hello = sync::statedFormat(1);
};

// The line nexus-server new prints. `format` 0 states none: the line a window that stated no format reads (one from
// before formats, which takes nothing after the key). `hello`, the server's Hello as sync::helloText, goes after the
// format, only to a window that stated a Hello: a window from before Hello reads nothing after the format.
std::string connectLine(const std::string& port, const std::string& key, std::uint32_t format = 0,
                        const std::string& hello = std::string());
// True for exactly a CONNECT line: "NEXUS CONNECT", a port of 1 to 65535, and a key of 22 base64 characters (mosh's
// printable 128-bit key), then, optionally, "format" and a number from 1, then, optionally, "hello" and a Hello
// (sync::readHello), separated by single spaces.
bool parseConnect(const std::string& line, Connect* out);

// The words of a client's server command (--server-command, the client config's `server-command`): what it runs on the
// server's machine in place of `nexus-server new`, as the mock's `/usr/bin/env HOME=<home> <nexus-server> new --dir <dir>`
// (docs/mock.md). Words are separated by spaces or tabs, with no quoting. False, with why, when it has no word.
bool serverCommandWords(const std::string& text, std::vector<std::string>* words, std::string* why);

// The line ssh runs on the server's machine: `exec "$SHELL" -l -c '<command>'`, each word of the command quoted.
std::string remoteLine(const std::vector<std::string>& command);

struct Bootstrap {
    std::string ssh = "/usr/bin/ssh";
    std::string host;                                           // as ssh takes it
    std::vector<std::string> command = {"nexus-server", "new"}; // run there in the login shell
    int timeoutSeconds = 30;
    // The state format the window reads, stated to nexus-server new in its environment (sync/formats.h); 0 states
    // none, as a window before formats did. From format 4 (kTablesFormat) the window also states its Hello, the format
    // as its proto and sync::kMinPeer; a format below 4 states no Hello, as a window from before Hello did.
    std::uint32_t stateFormat = sync::kStateFormat;
    // The window's client id and the number of its first action in the session: `--client <id> --next <n>` after the
    // command (server/server.h), so that a session made again sends again, under their own numbers, the actions it has
    // no acknowledgement for, and the server skips those it applied. Empty: the window names itself not.
    std::string client;
    std::uint64_t next = 1;
    // Ask only a running server, and start none: kAskRunningVariable=1 in nexus-server new's environment, which a
    // server from before it ignores (it starts one when none runs, as before). The window's link sets it once it has
    // heard a server (sync/redial.h).
    bool askRunning = false;
    // Run the command on this machine, in the login shell, with no ssh (`/bin/sh -c` and the line ssh would run):
    // nexus-link-relay's --local, for the window on Windows whose server is the one in WSL2, beside the relay. The host
    // is then only a name for messages.
    bool local = false;
};

// The variable that tells nexus-server new to ask only a running server.
inline const char* const kAskRunningVariable = "NEXUS_ASK_RUNNING";

// The Hello the bootstrap states, if it states one.
bool statesHello(const Bootstrap& b, sync::Hello* out);

// The ssh command that runs it (with local, /bin/sh -c and the same line): no terminal, never a password prompt (BatchMode: the window has no terminal to ask in).
// The command runs under `env NEXUS_STATE_FORMAT=<n>` when the bootstrap states a format, `NEXUS_HELLO=<hello>` when
// it states a Hello, and `NEXUS_ASK_RUNNING=1` with askRunning; `--client <id> --next <n>` follows it when the
// bootstrap names a client.
Command bootstrapCommand(const Bootstrap& b);

// Runs the command over ssh with `run` and reads its output for the CONNECT line. False, with why (what ssh or the
// command printed on standard error among it, never the key), when ssh fails or prints no CONNECT line.
bool bootstrap(const Bootstrap& b, const CommandRunner& run, Connect* out, std::string* why);

// The numeric address of the host ssh reaches: ssh's own HostName for it (`ssh -G host`, run with `run`), resolved,
// IPv4 first.
bool resolveHost(const Bootstrap& b, const CommandRunner& run, std::string* ip, std::string* why);

} // namespace nexus::link
