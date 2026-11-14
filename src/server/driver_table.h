// Which program is each friend's driver: the file `drivers` in nexus-server's directory, read at each prompt, so that
// an edit takes effect without a restart (a driver running keeps the program it was started with; a friend the file no
// longer names has its prompts refused with the fix). Two kinds of line, words split on spaces and tabs (no quoting),
// `#` starting a comment line:
//
//   driver <harness> <program> [<argument>...]    the harness's driver, started with these arguments, then --cwd
//                                                  and --resume (docs/driver-protocol.md)
//   friend <name> <harness> [<directory>]         the friend's harness, and the directory it works in (--cwd): the
//                                                  home directory unless given; a leading ~/ is the home directory
//
// The server learns tabs by name only (the wire carries no harness), so the file names each friend's harness too.
// Pure: parsing and lookup, no files.

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "accounts/registry.h"

namespace nexus::server {

struct DriverTable {
    struct Friend {
        std::string harness;
        std::string cwd;
    };
    std::map<std::string, std::vector<std::string>> drivers;  // harness: program and arguments
    std::map<std::string, Friend> friends;
    std::vector<std::string> errors;                           // "line <n>: <why>", one per line not understood
};

// The table in `text`. `home` is the home directory, for a friend with no directory or one starting ~/.
DriverTable parseDriverTable(std::string_view text, const std::string& home);

// A friend's driver: its command and directory, or (problem not empty) why it has none and how to give it one. `path`
// is the file's path, for the fix.
struct ResolvedDriver {
    std::vector<std::string> command;
    std::string cwd;
    std::string problem;
};

ResolvedDriver resolveDriver(const DriverTable& table, const std::string& name, const std::string& path);

// The accounts and friends the table implies, for a server that had none (accounts/registry.h): an account for each
// driver line that passes `--account <name>` (its provider the line's harness); each friend, linked to its harness's
// account as its default when there is one, so that its sessions run on the account they ran on before; and each
// friend's one instance, named and placed on `machine` (this server's host), in the friend's directory. Names
// that are not names (accounts::isName) are left out.
accounts::Registry accountsFromDrivers(const DriverTable& table, const std::string& machine);

} // namespace nexus::server
