// Which program is each AI's driver: the file `drivers` in nexus-server's directory, read at each prompt, so that
// an edit takes effect without a restart (a driver running keeps the program it was started with; an AI the file no
// longer names has its prompts refused with the fix). Words split on spaces and tabs (no quoting),
// `#` starting a comment line:
//
//   driver <harness> <program> [<argument>...]    the harness's driver, started with these arguments, then --cwd
//                                                  and --resume (docs/driver-protocol.md)
//   signin <provider> <absolute program> [<argument>...]  enrollment helper, started with --account <name>
//   ai <name> <harness> [<directory>]         the AI's harness, and the directory it works in (--cwd): the
//                                                  home directory unless given; a leading ~/ is the home directory
//   primary <harness> <account> <absolute workspace>     the installing harness; the workspace is the rest of the line
//   primary-driver <harness> <absolute program> [<arguments>...]    its own command, independent of named AIs
//   primary-resume <harness session ID>           optional existing session verified by the installer
// For primary-driver and signin, a JSON array of strings can replace the program and arguments (paths with spaces).
//
// The server learns tabs by name only (the wire carries no harness), so the file names each AI's harness too.
// Pure: parsing and lookup, no files.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "accounts/registry.h"

namespace nexus::sync { struct SessionInfo; }

namespace nexus::server {

// A drivers file written before the friend names became AI says `friend <name> ...` and `{friend}` (and
// `nexus_policy friend-driver`): read as `ai`, `{ai}` and `ai-driver`, so an older file runs unchanged. The word stays
// here only.
inline constexpr std::string_view kOldAILine = "friend";
inline constexpr std::string_view kOldAIPlaceholder = "{friend}";
inline constexpr std::string_view kOldAIDriver = "friend-driver";
inline bool isAILine(std::string_view word) { return word == "ai" || word == kOldAILine; }

struct DriverTable {
    struct AI {
        std::string harness;
        std::string cwd;
    };
    struct Primary {
        std::string harness, account, cwd, resume;
        std::string instance() const;
        bool matches(const sync::SessionInfo& info) const;
    };
    std::optional<Primary> primary;
    std::map<std::string, std::vector<std::string>> primaryDrivers;
    std::map<std::string, std::vector<std::string>> drivers;  // harness: program and arguments
    std::map<std::string, std::vector<std::string>> signins;  // provider: trusted account enrollment program
    std::map<std::string, AI> ais;
    std::vector<std::string> errors;                           // "line <n>: <why>", one per line not understood
};

// The table in `text`. `home` is the home directory, for an AI with no directory or one starting ~/.
DriverTable parseDriverTable(std::string_view text, const std::string& home);

// An AI's driver: its command and directory, or (problem not empty) why it has none and how to give it one. `path`
// is the file's path, for the fix.
struct ResolvedDriver {
    std::vector<std::string> command;
    std::string cwd;
    std::string problem;
};

ResolvedDriver resolveDriver(const DriverTable& table, const std::string& name, const std::string& path);

// The accounts and AIs the table implies, for a server that had none (accounts/registry.h): an account for each
// driver line that passes `--account <name>` (its provider the line's harness); each AI, linked to its harness's
// account as its default when there is one, so that its sessions run on the account they ran on before; and each
// AI's one instance, named and placed on `machine` (this server's host), in the AI's directory; each AI
// not limited ("limit": false), as its sessions ran before the field. Names
// that are not names (accounts::isName) are left out.
accounts::Registry accountsFromDrivers(const DriverTable& table, const std::string& machine);

} // namespace nexus::server
