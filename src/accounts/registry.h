// The accounts and friends nexus-server holds: the providers' accounts, the friends, and each friend's links to the
// accounts it may run on (in link order, one of them its default, or none). docs/accounts.md is the files' schema, for
// the person or the AI who edits them; models/Accounts.tla is the model the hub's decisions are checked against.
//
// An account is a login with one provider (the harness its sessions run: "claude", later others). It is set up on its
// own, and its credentials live under ~/.nexus/server/accounts/<name>/, never here: these files hold names only. A
// friend links to any number of accounts, of any providers, and an account serves any number of friends.
//
// Two files, each pretty-printed JSON a person can edit by hand (accounts.json and friends.json), with a "version"
// from the first release. Written by this code in one layout only: two-space indent, one field per line, keys in a
// fixed order, the lists in the order they hold, a trailing newline; so a save never reshuffles what a person wrote.
// Pure: text in, text out, no files.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::accounts {

// The version of the files this code writes, and the newest it reads. An older version is upgraded as it is read.
constexpr int kVersion = 1;

struct Account {
    std::string name;
    std::string provider;
    bool operator==(const Account&) const = default;
};

struct Friend {
    std::string name;
    std::vector<std::string> accounts;  // linked, in link order (the order a rotation goes through them)
    std::string preferred;              // the default account: one of `accounts`, or empty for none
    bool operator==(const Friend&) const = default;
};

struct Registry {
    std::vector<Account> accounts;      // in the file's order
    std::vector<Friend> friends;        // in the file's order
    bool operator==(const Registry&) const = default;

    const Account* account(std::string_view name) const;
    const Friend* friendNamed(std::string_view name) const;
    Friend* friendNamed(std::string_view name);
    // Whether the friend links to the account.
    bool linked(std::string_view friendName, std::string_view account) const;
};

// An account's or a friend's name: 1 to 64 letters, digits, '-', '_' or '.', not starting with '.' (the Claude
// driver's rule for an account, drivers/claude/src/account.ts). A provider's: 1 to 32 lowercase letters, digits or '-'.
bool isName(std::string_view name);
bool isProvider(std::string_view provider);

// Why the registry is not consistent, each problem with what to change and in which file; empty when it is: names and
// providers well formed, no name twice, every link to an account set up and each once, every default linked.
std::vector<std::string> problems(const Registry& registry);

// The files' text, in the one layout this code writes.
std::string accountsText(const Registry& registry);
std::string friendsText(const Registry& registry);

// What reading a file found.
struct Read {
    bool ok = false;
    std::vector<std::string> errors;   // each "<path>:<line>: <what is wrong>; <what to change>"
    int version = 0;                   // the version the file said
    bool upgraded = false;             // it was older than kVersion, and read as kVersion
};

// Reads the accounts file's `text` (`path` for the errors) into registry->accounts, or the friends file's into
// registry->friends. An empty or missing file (text empty) reads as none. Refused, with every error found: text that
// is not JSON (with the line where reading stopped), a version newer than kVersion or not a whole number, a field
// unknown or of the wrong kind, a name or provider malformed. Consistency across the two files is problems()'s.
Read readAccounts(std::string_view text, const std::string& path, Registry* registry);
Read readFriends(std::string_view text, const std::string& path, Registry* registry);

// The accounts the server picks, from those signed in (`up`):
// the friend's next signed-in linked account after `current`, in link order and cycling round, never `current`;
// empty when there is none (a rotation, at a usage limit).
std::string nextUp(const Registry& registry, const std::set<std::string>& up, std::string_view friendName,
                   std::string_view current);
// Where a session of the friend goes when its account is no longer linked to it: the friend's default when signed in,
// else its first signed-in linked account, else none (empty).
std::string landing(const Registry& registry, const std::set<std::string>& up, std::string_view friendName);

// What changed from `before` to `after`, one line each, for the apply's report (empty when nothing did).
std::vector<std::string> changes(const Registry& before, const Registry& after);

} // namespace nexus::accounts
