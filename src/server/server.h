// nexus-server: the server the window talks to, on the machine that keeps going when the laptop is closed. It keeps
// the friend tabs (sync/hub.h) and each window's applied counter on disk (server/durable.h, src/store), so a restart,
// or a kill, loses nothing it acknowledged; and serves each window over a mosh session of its own
// (link/mosh_link.h), several at once.
//
// Like mosh-server, it is reached in two steps. `nexus-server new`, which the window runs over ssh, asks the running
// server for a session over a Unix socket in the person's own directory (mode 0600, in a directory of mode 0700),
// starting the server first if none runs; prints the session's line, `NEXUS CONNECT <port> <key>`; and exits. The
// server itself (`nexus-server serve`) runs on: one thread, one poll loop over the control socket and every session's
// UDP socket.
//
// A window that names itself (`new --client <id> --next <n>`: an id of 1 to 64 letters, digits, '-' or '_' it keeps
// across sessions, and the number of the first action it will send) has its applied counter kept in the store: when it
// reconnects, after a server restart or a lost session, and sends again the actions it has no acknowledgement for
// under their own numbers, the server skips those it applied before. A window that does not name itself gets a
// counter for that session only. A new session for a client ends that client's earlier session.

#pragma once

#include <cstdint>
#include <string>

namespace nexus::server {

struct Paths {
    std::string dir;        // --dir, else $HOME/.nexus
    std::string socket;     // <dir>/server.sock: the control socket
    std::string log;        // <dir>/server.log: a server `new` started writes here
    std::string store;      // <dir>/store: the tabs and the applied counters (src/store)
};

// Who asks for a session: a client id (empty: none) and the number of its first action in the session.
struct Client {
    std::string id;
    std::uint64_t next = 1;
};

// An id of 1 to 64 bytes, each a letter, digit, '-' or '_'.
bool validClientId(const std::string& id);

// The paths under `dir` (empty: $HOME/.nexus). False, with why, when there is no HOME or the socket's path is longer
// than a Unix socket's address holds.
bool paths(const std::string& dir, Paths* out, std::string* why);

// The server: runs until `nexus-server stop`, SIGTERM or SIGINT. Returns the exit status.
int serve(const Paths& p);
// Asks the running server for a session, starting one (`self` serve) if none answers; prints its CONNECT line.
int newSession(const Paths& p, const std::string& self, const Client& client = {});
// Asks the running server to stop.
int stop(const Paths& p);

} // namespace nexus::server
