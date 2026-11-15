// nexus-server's home: everything the server keeps is in ~/.nexus/server (--dir gives another), beside the window's
// ~/.nexus/client (client_home.h).
//
//   config            the server's settings (config.h): `machine <name>`, this machine's name in the fleet
//   drivers           which program is each friend's driver (server/driver_table.h)
//   store/            the sessions and the applied counters (src/store)
//   server.sock       the control socket, mode 0600
//   server.log        the log of a server `new` started
//   nexus-config/     the accounts and friends: accounts.json and friends.json, a git repository of their own
//                     (server/config_repo.h, docs/accounts.md); no credential is ever in it
//   accounts/<name>/  one harness login per directory: the Claude driver's --account (drivers/claude), always
//                     under $HOME/.nexus/server, whatever --dir says; secrets only, which no AI's tool reaches
//                     (docs/secrets.md, "The token wall")
//   harness/claude/<name>/  the Claude driver's own state for that account (its CLAUDE_CONFIG_DIR), kept apart from
//                     the login so that the logins can be walled whole
//
// ~/.nexus is made mode 0700 by whichever of the window and the server starts first; the server makes ~/.nexus/server,
// mode 0700, itself. Pure.

#pragma once

#include <string>
#include <vector>

namespace nexus::server {

// The keys the server's config takes: machine, the name this machine has in the fleet, which a friend's instance
// here names (accounts/registry.h; by default the short form of its host name); and claude, the absolute path of the
// `claude` program in-app sign-in runs (`claude setup-token`; by default the first `claude` on the server's PATH).
inline const std::vector<std::string> kServerConfigKeys{"machine", "claude"};

// The server's settings, as its config gives them (empty: not given).
struct ServerSettings {
    std::string machine;
    std::string claude;
};

// The machine this server runs on, by the name a friend's instance gives it: `configured` (the config's machine) when
// set, else the short form of `hostName` (gethostname's, cut at its first '.'; empty when gethostname failed); empty
// when neither gives one, and then no instance is taken to be on this machine.
std::string serverMachine(const std::string& configured, const std::string& hostName);

// Whether a friend's instance on `machine` runs on this server's machine: the same name, in any case, as `self`
// (serverMachine(): the config's machine, else the short host name) or as `hostName`, the full host name
// (gethostname's, as `studio.local`). An empty name matches nothing.
bool sameMachine(const std::string& machine, const std::string& self, const std::string& hostName = std::string());

// Where the harness logins are, under HOME whatever --dir says: the one path the server's sign-in check and the Claude
// driver both read (drivers/claude/src/account.ts accountsDir, held to this line by its unit test).
inline const char* const kAccountsUnderHome = ".nexus/server/accounts";

// `home` (HOME, without a trailing '/') with kAccountsUnderHome: the directory of the account logins, one per name.
std::string accountsDir(const std::string& home);

// The variable the Claude driver finds the sandbox's bwrap and socat by on Linux (drivers/claude/src/tools.ts).
inline const char* const kSandboxBinVariable = "NEXUS_SANDBOX_BIN";

// Where the sandbox's bwrap and socat are for the nexus-server at the absolute path `self`: the folder `sandbox` the
// build made beside it (cmake/sandbox.cmake). Only ever from the server's own path, never from the environment (the
// driver loads an AppArmor profile for what is there, through sudo). Empty when `self` is not an absolute path to a
// file: the server then unsets the variable rather than keep one it was started with.
std::string sandboxBinOf(const std::string& self);

// `home` (HOME) with .nexus/server added in `dir`, the ~/.nexus above it in `top`, and HOME itself, without a trailing
// '/', in `resolved`: the one place the server resolves its home. False, with why, when `home` is null or empty.
bool serverHome(const char* home, std::string* resolved, std::string* top, std::string* dir, std::string* why);

// Reads the server's config, `text` (the file at `path`), into `out`. False, with every error in `why` (each naming the
// file and the line), when a line is malformed or a machine is not a host name; `warnings` gets the rest, one line each.
bool readServerConfig(const std::string& text, const std::string& path, ServerSettings* out,
    std::vector<std::string>* warnings, std::string* why);

} // namespace nexus::server
