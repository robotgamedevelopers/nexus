// Up, client to server: the person's actions as an append-only numbered list, the state mosh's Transport carries from
// the window to nexus-server (src/link). The client is its only writer. Each action gets the next number, 1, 2, 3 and
// so on, and keeps it; the server applies each number once, in order (Applier), however often or out of order mosh
// delivers the diffs that carry it. models/Link.tla is the model; tests/sync/link_walk.cpp binds this code to it.
//
// mosh's interface (src/network/transportsender-impl.h, networktransport-impl.h in mosh 1.4.0): the transport copies
// whole states all the time, so a copy is one shared pointer; it calls subtract(acknowledged state) on every state it
// keeps, so a state holds only the actions not yet known to be received, while its numbers stay absolute.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sync/sessions.h"

namespace nexus::sync {

// Each action names a session by its id (sync/sessions.h), never a tab: a window keeps which session each of its tabs
// shows to itself. What an action does is decided by the server, against the sessions it holds when it applies it.
enum class ActionKind : std::uint8_t {
    Send = 1,       // a prompt sent in a session: it goes in the transcript
    Queue = 2,      // a prompt queued in a session, for later
    Stop = 3,       // stop the friend's work in a session (no text): applied at once, never queued behind anything
    Create = 4,     // a new session with a friend (friendName): its account, model and effort copied from `session`,
                    // or, with session 0, the friend's defaults
    Ensure = 5,     // a session with a friend (friendName) only if the friend has none: what a window sends for each
                    // friend it shows, so that each has a session to show
    SetAccount = 6, // the session's account, model or effort becomes `text` (empty: the harness's default), from its
    SetModel = 7,   // next turn on
    SetEffort = 8,
    // The accounts and friends (accounts/registry.h, docs/accounts.md), decided by the server against its own:
    AddAccount = 9,     // an account (`account`) set up with its provider (`text`)
    RemoveAccount = 10, // an account (`account`) removed: every link to it goes, and its sessions move off it
    AddFriend = 11,     // a friend (friendName) set up, with no accounts
    LinkAccount = 12,   // the friend (friendName) may run on the account (`account`), after those it has
    UnlinkAccount = 13, // the friend no longer runs on the account: its sessions on it move off it
    SetDefault = 14,    // the friend's default account becomes `account` (empty: none)
};

struct Action {
    std::uint64_t num = 0;  // its place in the list, from 1, never reused
    ActionKind kind = ActionKind::Send;
    SessionId session = 0;  // the session it names; a create's to copy from (0: none); an ensure's is 0
    std::string friendName; // a create's and an ensure's friend; empty for the rest
    std::string text;
    std::string account;    // the account an account action names; empty for the rest (and not on the wire for them)
    bool operator==(const Action&) const = default;
};

class Actions {
public:
    Actions();
    // An empty list whose next action is numbered `next` (1 if 0): the state 0 both ends of a session start from when
    // a window, reconnecting, goes on with its numbers (link/mosh_link.h). Both ends must be given the same number.
    explicit Actions(std::uint64_t next);

    // The client's one write: appends `action` (its num ignored) with the next number and returns that number.
    std::uint64_t add(Action action);

    std::uint64_t first() const { return first_; }              // the number of the oldest action held
    std::uint64_t next() const { return first_ + held_->size(); } // the number the next action gets
    const std::vector<Action>& held() const { return *held_; }  // actions first() .. next() - 1

    // mosh's state interface.
    // The actions numbered existing.next() .. next() - 1, encoded; empty when there are none. Throws std::logic_error
    // if this state no longer holds them (existing is from before what this state subtracted) or existing is ahead of
    // this state: mosh never asks either (its sender diffs from states it sent after the one it subtracted).
    std::string diff_from(const Actions& existing) const;
    std::string init_diff() const;
    // Appends the actions a diff carries that this state lacks; skips those it already passed. Throws BadDiff, changing
    // nothing, on malformed bytes or a diff that starts after next() (a gap).
    void apply_string(const std::string& diff);
    bool operator==(const Actions& x) const;
    bool compare(const Actions& x) const { return !(*this == x); } // mosh's verbose check: true when they differ
    // Forgets the actions numbered below prefix->next(); the numbers of the rest stay.
    void subtract(const Actions* prefix);
    void reset_input() {}

    // The actions a diff carries, for the server, which reads them out of mosh's get_remote_diff(). Throws BadDiff.
    static std::vector<Action> parse(const std::string& diff);

private:
    std::uint64_t first_ = 1;
    std::shared_ptr<const std::vector<Action>> held_;
};

// The server's side of one client's list: applies each action exactly once, in number order. apply() takes the actions
// a diff carried (Actions::parse); those numbered at or below applied() were applied before and are skipped. Returns the
// actions newly applied, in order. A list that starts past applied() + 1 would skip an action: it is refused whole,
// nothing is applied, and `refused` (when given) says why.
class Applier {
public:
    // `applied`: the number of the client's last action applied before this session (the store's counter).
    explicit Applier(std::uint64_t applied = 0) : applied_(applied) {}
    std::uint64_t applied() const { return applied_; }
    std::vector<Action> apply(const std::vector<Action>& actions, std::string* refused = nullptr);

private:
    std::uint64_t applied_ = 0;
};

} // namespace nexus::sync
