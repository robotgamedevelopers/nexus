// nexus-server's home: everything the server keeps is in ~/.nexus/server (--dir gives another), beside the window's
// ~/.nexus/client (client_home.h).
//
//   config            the server's settings (config.h); it takes none yet
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

// The keys the server's config takes: none yet.
inline const std::vector<std::string> kServerConfigKeys{};

// `home` (HOME) with .nexus/server added in `dir`, the ~/.nexus above it in `top`, and HOME itself, without a trailing
// '/', in `resolved`: the one place the server resolves its home. False, with why, when `home` is null or empty.
bool serverHome(const char* home, std::string* resolved, std::string* top, std::string* dir, std::string* why);

// Reads the server's config, `text` (the file at `path`). False, with every error in `why` (each naming the file and the
// line), when a line is malformed; `warnings` gets the rest, one line each.
bool readServerConfig(const std::string& text, const std::string& path, std::vector<std::string>* warnings,
    std::string* why);

} // namespace nexus::server
