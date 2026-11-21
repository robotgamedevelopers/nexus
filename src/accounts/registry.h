// The accounts and friends nexus-server holds: the providers' accounts, the friends, and each friend's links to the
// accounts it may run on (in link order, one of them its default, or none). docs/accounts.md is the files' schema, for
// the person or the AI who edits them; models/Accounts.tla is the model the hub's decisions are checked against.
//
// An account is a login with one provider (the harness its sessions run: "claude", later others). It is set up on its
// own, and its credentials live under ~/.nexus/server/accounts/<name>/, never in the registry. A
// friend links to any number of accounts, of any providers, and an account serves any number of friends.
//
// Two files, each pretty-printed JSON a person can edit by hand (accounts.json and friends.json), with a "version"
// from the first release. Written by this code in one layout only: two-space indent, one field per line, keys in a
// fixed order, the lists in the order they hold, a trailing newline; so a save never reshuffles what a person wrote.
// Pure: text in, text out, no files.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::accounts {

// The version written for friends and for accounts using the original fields. Version 1 upgrades to this.
// Version 2 added an account's "billing" and a friend's "limit" (docs/accounts.md, Versions): a version 1 file reads
// every account as billed by subscription and every friend as not limited, which is how nexus ran them then.
constexpr int kVersion = 2;
// accounts.json needs version 3 only when storing picker metadata or an on-use login.
constexpr int kAccountOptionsVersion = 3;

// How an account's sessions are paid for: a subscription's included usage, or the provider's API rate. A session on an
// API-billed account is always limited (Friend::limit), whatever its friend says (docs/accounts.md, "limit").
enum class Billing { Subscription, Api };

// Count cap for all file versions. The expanded picker metadata and links also have a wire and decode
// budget, checked by sync::accountRosterProblem before a registry is published.
constexpr std::size_t kMaxFriends = 256;

struct Account {
    std::string name;
    std::string provider;
    Billing billing = Billing::Subscription;
    std::string label{};     // optional picker text; the name remains the account id and credential directory
    bool signInOnUse = false;
    std::string email{};     // configured address shown to linked windows; independent of provider identity verification
    bool operator==(const Account&) const = default;
};

// An instance of a friend: that friend running on one machine, from one working directory (rowan on studio in
// /Volumes/nova/ai/rowan, rowan on studio in a rocketnet checkout, rowan on the macbook). A friend's identity and its
// accounts are the friend's, shared by all its instances; a session belongs to one instance. The machine is a fleet
// host name, the one it runs on now: moving it is an ordinary edit.
struct Instance {
    std::string name;       // unique within its friend
    std::string machine;    // a host name, as `studio`
    std::string dir;        // an absolute path on that machine
    bool operator==(const Instance&) const = default;
};

struct Friend {
    std::string name;
    std::vector<std::string> accounts;  // linked, in link order (the order a rotation goes through them)
    std::string preferred;              // the default account: one of `accounts`, or empty for none
    std::vector<Instance> instances{};  // in the file's order; the first is the friend's default instance
    // Limited: the AI's file tools and shell read and write only inside its instance's directory (the driver's sandbox,
    // docs/secrets.md "A limited friend"). True for a friend made now; a friend from before the field (a version 1
    // file, or the drivers file's) is false, as it ran then. Never false while the friend links an API-billed account.
    bool limit = true;
    bool operator==(const Friend&) const = default;

    const Instance* instance(std::string_view instanceName) const;
};

struct Registry {
    std::vector<Account> accounts;      // in the file's order
    std::vector<Friend> friends;        // in the file's order
    // friends.json's "limit_keeps_out": more directories a limited friend's AI is kept out of, on top of the ones the
    // driver always keeps out (the home directory and the places people keep files: docs/accounts.md "limit"), as a
    // second data volume. It only adds: nothing here makes a limited friend read more. In the file's order.
    std::vector<std::string> keepsOut{};
    bool operator==(const Registry&) const = default;

    const Account* account(std::string_view name) const;
    const Friend* friendNamed(std::string_view name) const;
    Friend* friendNamed(std::string_view name);
    // Whether the friend links to the account.
    bool linked(std::string_view friendName, std::string_view account) const;
};

// An account's or a friend's name: 1 to 64 letters, digits, '-', '_' or '.', not starting with '.' or '-' (the Claude
// driver's rule for an account, drivers/claude/src/account.ts). A provider's: 1 to 32 lowercase letters, digits or '-'.
bool isName(std::string_view name);
bool isProvider(std::string_view provider);
// A machine's name: 1 to 253 letters, digits, '-' or '.', not starting with '-' or '.' (a host name, never an address with a
// user or a port). A directory: an absolute path, of printable characters, at most 4096 bytes.
bool isMachine(std::string_view machine);
bool isDir(std::string_view dir);

// Why the registry is not consistent, each problem with what to change and in which file; empty when it is: names and
// providers well formed, no name twice, every link to an account set up and each once, every default linked, every instance named once
// within its friend, on a machine, in an absolute directory, no friend with "limit": false linked to an account
// billed at API rate, and each "limit_keeps_out" directory absolute, not the root, and there once.
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
// registry->friends (and its "limit_keeps_out" into registry->keepsOut). An empty or missing file (text empty) reads
// as none. Refused, with every error found: text that
// is not JSON (with the line where reading stopped), an unsupported version or one that is not a whole number, a field
// unknown or of the wrong kind, a name or provider malformed, more than kMaxFriends friends (in any version).
// Consistency across the two files is problems()'s.
// In a version 1 friends file, a friend without "limit" reads unlimited (it ran so) only when `committed` is null (the
// committed files themselves are being read) or names it; a friend `committed` does not hold, such as one added by
// hand while the file is still version 1, reads limited.
Read readAccounts(std::string_view text, const std::string& path, Registry* registry);
Read readFriends(std::string_view text, const std::string& path, Registry* registry,
                 const Registry* committed = nullptr);

// The accounts the server picks, from those signed in (`up`):
// the friend's next signed-in linked account after `current`, in link order and cycling round, never `current`;
// empty when there is none (a rotation, at a usage limit).
std::string nextUp(const Registry& registry, const std::set<std::string>& up, std::string_view friendName,
                   std::string_view current);
// Where a session of the friend goes when its account is no longer linked to it: the friend's default when signed in,
// else its first signed-in linked account, else none (empty).
std::string landing(const Registry& registry, const std::set<std::string>& up, std::string_view friendName);

// Whether a session of `friendName` on `account` (empty: none) runs limited, decided when its driver starts: when the
// friend is limited, or the account is billed at API rate, or the registry is not there or does not hold the friend
// (deny-leaning: a friend nexus cannot read a "limit" for is limited). models/Accounts.tla `Eff` and `Start`
// (ApiAlwaysLimited, NeverWeakerThanFriend; tests/accounts/accounts_walk.cpp binds it).
bool limitedFor(const Registry* registry, std::string_view friendName, std::string_view account);

// The words of a billing in the files ("subscription", "api").
std::string_view billingName(Billing billing);

// What changed from `before` to `after`, one line each, for the apply's report (empty when nothing did).
std::vector<std::string> changes(const Registry& before, const Registry& after);

} // namespace nexus::accounts
