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

#include "sync/secrets.h"
#include "sync/sessions.h"

namespace nexus::sync {

// Each action names a session by its id (sync/sessions.h), never a tab: a window keeps which session each of its tabs
// shows to itself. What an action does is decided by the server, against the sessions it holds when it applies it.
//
// Across versions (docs/formats.md): up to state format 2 the actions are unframed, kinds 1 to 14. From format 3 each
// action is its kind and then its fields framed by their length, so that a reader skips the fields it does not know and
// reads an action of a kind it does not know as Unknown (counted, never applied). From format 4 they are schema's
// ActionsDiff table (schema/Nexus.schema), with the same rules by the table's own. The secret dialog's two kinds come
// with format 3 (sync/secrets.h); a window sends them only for a request it received, which only a server of format 3
// or later sends. The sign-in's three come with format 5 (sync/signin.h): a window sends them only for a flow it
// received, which a server sends only on a link of 5 or later; formats 1 to 4 carry none.
enum class ActionKind : std::uint8_t {
    Unknown = 0,    // read, never written: a kind from a later window, which the server counts and does not apply
    Send = 1,       // a prompt sent in a session: it goes in the transcript
    Queue = 2,      // a prompt queued in a session, for later
    Stop = 3,       // stop the AI's work in a session (no text): applied at once, never queued behind anything
    Create = 4,     // a new session with an AI (aiName): its account, model and effort copied from `session`,
                    // or, with session 0, the AI's defaults
    Ensure = 5,     // a session with an AI (aiName) only if the AI has none: what a window sends for each
                    // AI it shows, so that each has a session to show
    // A SetAccount text beginning with "+" names a command in server account policy. Real account IDs cannot
    // start with "+". A window sends these commands only when the server advertises linked accounts.
    SetAccount = 6, // the session's account, model or effort becomes `text` (empty: the harness's default), from its
    SetModel = 7,   // next turn on
    SetEffort = 8,
    // The accounts and AIs (accounts/registry.h, docs/accounts.md), decided by the server against its own:
    AddAccount = 9,     // an account (`account`) set up with its provider (`text`)
    RemoveAccount = 10, // an account (`account`) removed: every link to it goes, and its sessions move off it
    AddAI = 11,     // an AI (aiName) set up, with no accounts
    LinkAccount = 12,   // the AI (aiName) may run on the account (`account`), after those it has
    UnlinkAccount = 13, // the AI no longer runs on the account: its sessions on it move off it
    SetDefault = 14,    // the AI's default account becomes `account` (empty: none)
    // The secret dialog (sync/secrets.h), from format 3:
    SecretAnswer = 15,  // the value the person pasted (`secret`) for request `request`
    SecretCancel = 16,  // the person cancelled request `request` (Escape)
    // In-app sign-in (sync/signin.h), from format 5 (schema's ActionKind SignInClaim, SignInOpened, SignInCallback); a
    // server of 4 reads each as Unknown and counts it, a link below 5 carries none, and the account stays needing
    // sign-in:
    SignInClaim = 17,   // the window claims sign-in flow `request`, by its claim token (`text`)
    SignInOpened = 18,  // the window holding flow `request` (`text`, its claim token) opened its URL in the browser
    SignInCallback = 19,// the window's gate took flow `request`'s callback: its query (`secret`), from `text`'s window
    SignOut = 20,       // Claude logout, only when session still selects the exact named account
    ResolveLimit = 21, // accept a paused turn's offered target (aiName) for account, request and choice kind (text)
    // The code a vendor's sign-in page showed (`secret`), pasted into the window holding flow `request` (`text`, its
    // claim token), from the tab of session `session` (sync/signin.h signsInByPastedCode). A server from before it
    // reads it as Unknown and counts it; nothing of it is kept.
    SignInCode = 22,
};

// The last kind this build knows.
constexpr ActionKind kLastActionKind = ActionKind::SignInCode;

struct Action {
    std::uint64_t num = 0;  // its place in the list, from 1, never reused
    ActionKind kind = ActionKind::Send;
    SessionId session = 0;  // the session it names; a create's to copy from (0: none); an ensure's is 0
    std::string aiName; // a create's and an ensure's AI; empty for the rest
    std::string text;
    std::string account;    // the account an account action names; empty for the rest (and not on the wire for them)
    std::uint64_t request = 0;  // a secret answer's or cancel's request id; 0 for the rest
    SecretValue secret{};   // a secret answer's value; empty for the rest. Wiped when the action is freed, which is
                            // when mosh's acknowledgement subtracts it (Actions::subtract)
    bool pastesCode = false;    // a claim's: the window takes a vendor's pasted code in its tab (SignInCode); a window
                                // from before leaves it false, and is refused a flow that needs one (Hub, SignInClaim)
    bool operator==(const Action&) const = default;
};

// What an action is, for a log or a test's message: its number, its kind and, for a secret's, its request id. Never its
// text or its value.
std::string describe(const Action& action);

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

    // The format (sync/formats.h) diffs are written and read in: the link's, agreed when it is made. A copy keeps it;
    // equality ignores it. Up to format 2 a secret's action, which that format cannot carry, is written as a stop
    // naming session 0, which every server refuses and counts: its value never leaves the window.
    std::uint32_t format() const { return format_; }
    void setFormat(std::uint32_t format) { format_ = format; }

    // Shares a close flag across copies of this state (mosh copies remote states from initial_remote): once closed,
    // apply_string refuses new diffs so a stopping server's shutdown loop never acknowledges actions after its last
    // store flush.
    void trackClose() { closed_ = std::make_shared<bool>(false); }
    void close() {
        if (closed_) {
            *closed_ = true;
        }
    }

    // The actions a diff carries, for the server, which reads them out of mosh's get_remote_diff(). Throws BadDiff.
    // `unknown` (when given) is how many fields and kinds of a later window's it skipped (from format 4), for the log.
    static std::vector<Action> parse(const std::string& diff, std::uint32_t format = kStateFormat,
                                     int* unknown = nullptr);

private:
    std::uint64_t first_ = 1;
    std::shared_ptr<const std::vector<Action>> held_;
    std::uint32_t format_ = kStateFormat;
    std::shared_ptr<bool> closed_;
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
