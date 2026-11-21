// Down, server to client: the sessions as nexus-server holds them, the state mosh's Transport carries from nexus-server
// to the window (src/link). The server is its only writer. Per session: its id, what it is (the friend it is with, its
// harness, account, model and effort), the tail of its transcript (the newest entries, where an entry may grow until
// it is closed) and its queue. Nothing in it names a tab, a selection or an order the window shows: which session a
// tab shows is the window's own business (sync/mirror.h). models/Link.tla is the model; tests/sync/link_walk.cpp binds
// this code to it.
//
// Cheap to copy: a state is one shared pointer to an immutable list of immutable sessions, and a session's entries are
// shared immutable entries, so the server's store, every state mosh keeps and every client's copy share one copy of
// each entry. A change makes a new list and a new session for the session it touches; the rest are shared.
//
// Bounded diffs: a state holds only each transcript's tail (Tail: the newest entries up to a count and a byte budget,
// always the newest entry whole), so a diff from a state however far behind sends at most each session's tail and its
// queue, never the whole history. The client keeps what it has seen (sync/mirror.h) and marks the entries it never
// saw. The diff from a state that holds an entry still open sends only the text that entry grew by.
//
// Across versions: a diff is written in the format of the link it goes on (sync/formats.h, docs/formats.md), which a
// window and a server of different versions agree on when the link is made: from format 4 schema's SessionsDiff table
// (schema/Nexus.schema), before it the hand-written formats (sync/codec.h). What a diff says is worked out here, the
// same in every format; the codec only writes it and reads it back. An entry kind this build does not know shows as a
// notice and an activity it does not know as busy; neither ever fails the read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "model.h"
#include "sync/formats.h"
#include "sync/secrets.h"
#include "sync/signin.h"

namespace nexus::sync {

// A session's id: minted by the server from 1, never reused, the one name a session has on the wire and in the store.
using SessionId = std::uint64_t;

// What a session is, apart from its transcript and queue: the friend it is with (an attribute, so that a window can
// group sessions by friend; it names no tab), the harness its driver runs (Claude, for now), the account the driver
// runs under, the model and effort each turn is sent with, and the friend's instance it belongs to (the machine and
// directory it runs in), so that a window can also group sessions by instance. An empty account, model or effort is
// the harness's default.
struct SessionInfo {
    std::string friendName;
    std::string harness;
    std::string account;
    std::string model;
    std::string effort;
    std::string instance{};     // which of its friend's instances (accounts/registry.h): a machine and a directory; empty
                                // before the server holds the friend's instances, or when the friend has none
    bool operator==(const SessionInfo&) const = default;
};

struct SessionEntry {
    EntryKind kind = EntryKind::Notice;
    std::string text;
    bool closed = true;     // an open entry may still grow by text appended to its end; a closed one never changes
    bool operator==(const SessionEntry&) const = default;
};

using EntryRef = std::shared_ptr<const SessionEntry>;

struct AccountChoice {
    std::string name;
    std::string label{};
    bool needsSignIn = false;    // the server can sign this linked account in and has no login for it
    bool operator==(const AccountChoice&) const = default;
};

// A friend the server holds (accounts/registry.h, its friends.json): what a window needs to show the friend's tab and
// to start a session with it (an ensure naming it). The server is the authority on who the friends are; the window
// shows them. A later version adds fields here and in schema/Nexus.schema's Friend.
struct ServerFriend {
    std::string name;
    std::vector<AccountChoice> accounts{};   // in link order, empty with an older server
    bool operator==(const ServerFriend&) const = default;
};

// What the friend of a session is doing, as the server holds it now (server/supervisor.h Supervisor::activity): Busy
// while a turn is open or about to begin (a prompt waits, the driver starts, a stop is out); Idle when nothing runs and
// nothing will until the person sends (every queued prompt taken); Down when the friend cannot run (its driver crashed,
// failed to start too many times in a row, or has no login) until the person's next prompt. In memory only, never in
// the store: a server that starts again works it out afresh. The window's state cursor shows it (sync/client.h).
enum class Activity : std::uint8_t { Idle = 0, Busy = 1, Down = 2 };

struct SessionView {
    SessionId id = 0;
    SessionInfo info;
    Activity activity = Activity::Idle;
    std::uint64_t first = 0;            // the transcript index (from 0) of entries[0]
    std::vector<EntryRef> entries;      // the transcript's tail, oldest first, through its newest entry
    std::vector<std::string> queue;     // prompts queued for later, oldest first
    std::uint64_t total() const { return first + entries.size(); }  // the transcript's length
    bool operator==(const SessionView& x) const;
};

using SessionRef = std::shared_ptr<const SessionView>;

// How much of a transcript a state carries: the newest entries, at most `entries` of them, while their text totals at
// most `bytes`; the newest entry always, whatever its size.
struct Tail {
    std::size_t entries = 16;
    std::size_t bytes = 16 * 1024;
};

// A session's tail of a whole transcript, by those rules; `open`, when given, is the newest entry, after the tail, and
// outside its rules (it is within the hub's text limit): the newest closed entry is always carried with it.
SessionView tailOf(SessionId id, SessionInfo info, const std::vector<EntryRef>& transcript,
                   std::vector<std::string> queue, Tail tail, EntryRef open = nullptr);

class Sessions {
public:
    Sessions();
    // The sessions in order of id, no two with one id and none with id 0; throws std::logic_error otherwise.
    explicit Sessions(std::vector<SessionRef> sessions);

    const std::vector<SessionRef>& sessions() const { return *sessions_; }
    // The open secret requests (sync/secrets.h), oldest first, after the sessions: from format 3 on the wire, never a
    // value. A window of an older format receives none.
    const std::vector<SecretRequest>& requests() const { return *requests_; }
    void setRequests(std::vector<SecretRequest> requests);
    // The server's friends, in its friends file's order, after the requests: from format 4 on the wire (schema's
    // SessionsDiff.friends), whole in each diff where they changed and in no other. friendsKnown() is false until the
    // server sets them: in a state from a server before them (or of an older format), and from a server that holds no
    // friends file it could read; a window then shows the friends of the sessions it holds (sync/client.h friendsOf).
    // Empty and known: the server has none.
    const std::vector<ServerFriend>& friends() const { return *friends_; }
    // The list itself, shared by every copy of the state and every state a diff that does not carry the friends is
    // applied to: one pointer until setFriends or clearFriends sets another, so that a window tells a new list from the
    // one it holds without comparing them (sync/friend_strip.h).
    const std::shared_ptr<const std::vector<ServerFriend>>& friendsList() const { return friends_; }
    bool friendsKnown() const { return friendsKnown_; }
    void setFriends(std::vector<ServerFriend> friends);
    void clearFriends();
    // The friends of `from`, known or not, its list shared and not copied.
    void shareFriends(const Sessions& from);
    // The live sign-in flows (sync/signin.h), by id, after the secret requests: schema's SessionsDiff.signins, on a
    // link of format 5 or later only (a window of 4 would skip them and never act). The URL is in an offer only in the
    // state of the link of the window that holds it (Hub::stateFor); never in a state file.
    const std::vector<SignInOffer>& offers() const { return *offers_; }
    void setOffers(std::vector<SignInOffer> offers);
    // First action from this client still unapplied when the server built this exact state. Zero on older links.
    std::uint64_t nextAction() const { return nextAction_; }
    void setNextAction(std::uint64_t next) { nextAction_ = next; }
    const SessionView* find(SessionId id) const;
    // The friend's first session: the one with the lowest id. Null when the friend has none.
    const SessionView* first(const std::string& friendName) const;

    // mosh's state interface.
    // Empty when the states are equal. Otherwise, for every session: unchanged, or what it is, its first index, and
    // from the first entry that `existing` does not hold as it is, each entry (the text it grew by, for one `existing`
    // holds open and that only grew), and its queue if that changed. A session `existing` has and this state lacks is
    // left out.
    std::string diff_from(const Sessions& existing) const;
    std::string init_diff() const;
    // Throws BadDiff, changing nothing, on malformed bytes or a diff from a state other than this one.
    void apply_string(const std::string& diff);
    // The format (sync/formats.h) diff_from writes and apply_string reads: a link's, agreed when it is made. A copy
    // keeps it; equality ignores it.
    std::uint32_t format() const { return format_; }
    void setFormat(std::uint32_t format) {
        format_ = format;
        if (format < kSignInFormat) nextAction_ = 0;
    }
    // How many fields and enum values the last diff applied held that this build does not know and skipped (a later
    // server's: schema's report.unknown, from format 4), so that the window can log them. A copy keeps it; equality
    // ignores it.
    int unknown() const { return unknown_; }
    bool operator==(const Sessions& x) const;
    bool compare(const Sessions& x) const { return !(*this == x); }  // mosh's verbose check: true when they differ
    void subtract(const Sessions*) {}   // a state is the whole picture, not a stream: nothing to forget
    void reset_input() {}

private:
    std::shared_ptr<const std::vector<SessionRef>> sessions_;
    std::shared_ptr<const std::vector<SecretRequest>> requests_;
    std::shared_ptr<const std::vector<ServerFriend>> friends_;
    bool friendsKnown_ = false;
    std::shared_ptr<const std::vector<SignInOffer>> offers_;
    std::uint32_t format_ = kStateFormat;
    int unknown_ = 0;
    std::uint64_t nextAction_ = 0;
};

// A state kept in a file, for `nexus --state` (a screenshot of what a server's state draws) and tests' fixtures: the
// line "nexus-state <format>" and the state's init_diff in that format, so the file says how to read it.
std::string stateFile(const Sessions& state);
// The state a file holds, read in its format (a later one than this build's as this build reads a later server's).
// False, saying why in one line, on bytes that are not a state file or a format older than this build's floor.
bool readStateFile(const std::string& bytes, Sessions* out, std::string* why);

} // namespace nexus::sync
