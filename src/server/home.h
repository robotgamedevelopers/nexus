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
//                     under $HOME/.nexus/server, whatever --dir says
//
// ~/.nexus is made mode 0700 by whichever of the window and the server starts first; the server makes ~/.nexus/server,
// mode 0700, itself. Pure.

#pragma once

#include <string>
#include <vector>

namespace nexus::server {

// The keys the server's config takes: machine, the name this machine has in the fleet, which a friend's instance
// here names (accounts/registry.h; by default the short form of its host name).
inline const std::vector<std::string> kServerConfigKeys{"machine"};

// The server's settings, as its config gives them (empty: not given).
struct ServerSettings {
    std::string machine;
};

// The machine this server runs on, by the name a friend's instance gives it: `configured` (the config's machine) when
// set, else the short form of `hostName` (gethostname's, cut at its first '.'; empty when gethostname failed); empty
// when neither gives one, and then no instance is taken to be on this machine.
std::string serverMachine(const std::string& configured, const std::string& hostName);

// Whether a friend's instance on `machine` runs on this server's machine, `self` (serverMachine()): the same name, in
// any case. Never when `self` is empty.
bool sameMachine(const std::string& machine, const std::string& self);

// `home` (HOME) with .nexus/server added in `dir`, the ~/.nexus above it in `top`, and HOME itself, without a trailing
// '/', in `resolved`: the one place the server resolves its home. False, with why, when `home` is null or empty.
bool serverHome(const char* home, std::string* resolved, std::string* top, std::string* dir, std::string* why);

// Reads the server's config, `text` (the file at `path`), into `out`. False, with every error in `why` (each naming the
// file and the line), when a line is malformed or a machine is not a host name; `warnings` gets the rest, one line each.
bool readServerConfig(const std::string& text, const std::string& path, ServerSettings* out,
    std::vector<std::string>* warnings, std::string* why);

} // namespace nexus::server
