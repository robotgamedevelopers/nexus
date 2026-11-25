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
//
// A prompt sent to a friend's tab goes to the friend's driver, a child process the server starts and supervises
// (server/drivers.h, server/supervisor.h, docs/driver-protocol.md); the file `drivers` in the directory says which
// program each friend's driver is (server/driver_table.h).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sync/formats.h"

namespace nexus::server {

struct Paths {
    std::string home;       // $HOME: a friend's ~ in the drivers file
    std::string top;        // $HOME/.nexus, which holds dir, made 0700 first; empty with --dir
    std::string dir;        // --dir, else $HOME/.nexus/server (server/home.h serverHome)
    std::string config;     // <dir>/config: the server's settings (config.h); it takes none yet
    std::string socket;     // <dir>/server.sock: the control socket
    std::string log;        // <dir>/server.log: a server `new` started writes here
    std::string store;      // <dir>/store: the sessions and the applied counters (src/store)
    std::string drivers;    // <dir>/drivers: which program is each friend's driver (server/driver_table.h)
    std::string accounts;   // $HOME/.nexus/server/accounts whatever --dir says: the logins, where the drivers read them
                            // (server/home.h accountsDir)
    std::string build;      // serve: the placed build this program runs from (server/builds.h holdBuild), learnt from
                            // its own resolved path; empty for a program outside one (a build tree)
    std::string self;       // this program's path (serve): in-app sign-in's helper, nexus-signin-open, is beside it
};

// Who asks for a session: a client id (empty: none), the number of its first action in the session, the state format
// the window reads (sync/formats.h; 0: it stated none, as a window before formats, which reads format 1), and its
// Hello, when it stated one (a window from before Hello states only the format).
struct Client {
    std::string id;
    std::uint64_t next = 1;
    std::uint32_t format = 0;
    bool hasHello = false;
    sync::Hello hello;
};

// The window's Hello: the one it stated, else that of a window from before Hello, which speaks its stated format.
sync::Hello helloOf(const Client& client);

// An id of 1 to 64 bytes, each a letter, digit, '-' or '_'.
bool validClientId(const std::string& id);

// The control socket's request for a session: "new", or "new <client> <next>", then " format <n>" when the window
// stated one, then " hello <hello>" (sync::helloText) when it stated a Hello. A server from before formats or before
// Hello refuses a request with a word it does not know; `new` then asks again without the Hello, then without the
// format, and prints that server's line, which states what that server speaks.
std::string newRequest(const Client& client);
// Reads a request newRequest makes; false for any other line.
bool parseNewRequest(const std::string& request, Client* out);
// The reply to a request for a session, before the link is made, decided on the window's Hello against the server's
// (sync::refusalOfWindow): empty when the window can be served, else the ERROR line that says, in one plain sentence,
// which end is too old (the server also logs it).
std::string formatRefusalReply(const Client& client, const sync::Hello& server = sync::kHello);

// The paths under `dir` (empty: $HOME/.nexus/server). False, with why, when there is no HOME (needed with --dir too) or
// the socket's path is longer than a Unix socket's address holds.
bool paths(const std::string& dir, Paths* out, std::string* why);

// The server: runs until `nexus-server stop`, SIGTERM or SIGINT, then ends every friend's driver and waits for them.
// Returns the exit status (server/start_order.h): 0 when another server answers that it is serving (it says so in one
// line), and after a stop; 3 when a writer held the store's lock for the whole wait and no server served; 1 when it
// could not start (readers that did not let go, a store that does not read, a socket that cannot be bound) or its store
// failed while it ran.
int serve(const Paths& p);
int servicesCommand(const Paths& p, const std::vector<std::string>& args);
// `nexus-server store check`: reads the store without changing it (store::Store::inspect, under its lock, shared) and
// prints one line of what it holds and what a server's open would do. Refused, in one line and exit 1, while a server
// has the store (it holds the lock): the store is not read under a writer.
int historyCommand(const Paths& p, const std::vector<std::string>& args);
int storeCommand(const Paths& p, const std::vector<std::string>& args);
// Asks the running server for a session, starting one (`self` serve) if none answers; prints its CONNECT line.
int newSession(const Paths& p, const std::string& self, const Client& client = {});
// Asks the running server to stop.
int stop(const Paths& p);
// Asks the running server to finish its open turns and exit with kDrainExit (server/drain.h).
int drain(const Paths& p);
// Asks the running server whether it serves, and from which build: prints its answer, `SERVING` and then
// `build <hash>` (or `build none`, a program outside the placed builds; ` draining` appended while draining). Exit 0
// when it serves, else 1.
int status(const Paths& p);
// `nexus-server config <verb> ...`: the accounts and friends (server/config_repo.h, docs/accounts.md). path, check,
// history and remote work on the files and the repository directly; reload and restore ask the running server, which
// decides.
int config(const Paths& p, const std::vector<std::string>& args);

} // namespace nexus::server
