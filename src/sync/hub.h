// nexus-server's sessions: what the clients' actions do to them, and the down state (sync/sessions.h) every client is
// sent. Every decision is the server's, made here against the sessions it holds: which session a create makes and
// with what, whether an ensure makes one, what a change of account, model or effort is, what a send, queue or stop to a
// session the server does not hold does (nothing: it is refused and counted). A window holds no state of its own but
// what it shows and what it prepares to send.
//
// A session's id is minted by the store (SessionStore::nextId): from 1, never reused, across restarts. A session is
// never removed. Its friend is an attribute: several sessions may be with one friend.
//
// A prompt sent in a session goes to its Responder: in nexus-server, the session's driver supervisor
// (server/drivers.h), which puts the prompt in the transcript and writes it to the friend's driver, whose replies it
// adds later (commit). Without a Responder (the tests, and the link's walk), the prompt is followed by a placeholder
// reply, "(received)", so that a round trip shows. A queued prompt goes in its queue (and to the Responder, which takes
// it from the queue's head at the end of a turn). A stop goes to the Responder at once, and changes no session of its
// own. A change of account, model or effort goes to the Responder once it is durable: it applies from the next turn.
//
// A friend's reply as it grows is a session's open entry (setOpen): the newest entry of the session's state, still
// open, kept in memory only and never in the store. When its block ends, the Responder commits the closed entry (and
// the open one goes): so the store holds each closed entry once, and a restart loses at most the open one.
//
// The sessions live in a SessionStore: MemoryStore keeps them in memory, for the tests and walks; nexus-server's
// DurableStore (server/durable.h) keeps them on disk (src/store). Each action is one step of the store: what it does to
// its session and the client's applied counter land together or not at all, so that after a restart the counter says
// exactly which of a reconnecting window's actions were applied. models/Link.tla is the model.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "sync/actions.h"
#include "sync/sessions.h"

namespace nexus::sync {

// What nexus-server keeps of a session's driver (server/supervisor.h) across restarts of the driver and of the server:
// the next turn number (from 1, never reused), the turn open (0 for none), the last session id the driver reported
// (the harness's own, the next start's --resume), and the prompts waiting for a driver, oldest first.
struct DriverState {
    std::uint64_t nextTurn = 1;
    std::uint64_t openTurn = 0;
    std::string session;
    std::vector<std::string> waiting;
    bool summary = false;       // the next send carries a summary of the conversation (server/supervisor.h setPlace)
    std::string sessionDir{};   // the directory the harness session was made in: resumed only there (empty: not known)
    bool operator==(const DriverState&) const = default;
};

// What the server holds of one session: what it is, its whole transcript, its queue, and its driver's state.
struct StoredSession {
    SessionInfo info;
    std::vector<EntryRef> transcript;
    std::vector<std::string> queue;
    DriverState driver;
};

// One change to a session: an entry at the end of its transcript; or (entry null) a prompt at the end of its queue;
// or (driver set) its driver's state, replaced whole; or (take set) the oldest prompt of its queue leaves it; or (info
// set) what it is, replaced whole: a step whose first change is an info makes the session, when the store does not
// hold it and its id is the store's next.
struct SessionChange {
    EntryRef entry;
    std::string prompt;
    std::optional<DriverState> driver = std::nullopt;
    bool take = false;
    std::optional<SessionInfo> info = std::nullopt;
};

// The take change: the oldest queued prompt leaves the queue.
SessionChange takeQueued();
// The info change.
SessionChange infoChange(SessionInfo info);

class SessionStore {
public:
    virtual ~SessionStore() = default;
    virtual const StoredSession* find(SessionId id) const = 0;
    virtual std::size_t size() const = 0;   // how many sessions it holds
    virtual std::vector<SessionId> ids() const = 0;     // in order
    // The id the next session made gets: one past every id ever minted, from 1. Durable: a restart never lowers it.
    virtual SessionId nextId() const = 0;
    // One step, all or none: `changes` to session `id`, and, when `client` is not empty, that client's applied counter
    // set to `num`. A session the store does not hold is made only by a step to id nextId() whose first change is an
    // info; a step with no changes names no session (id 0). False, with why, when the step was not taken; a store that
    // failed to write takes no later step either (failed()).
    virtual bool commit(SessionId id, const std::vector<SessionChange>& changes, const std::string& client,
                        std::uint64_t num, std::string* why) = 0;
    virtual std::uint64_t applied(const std::string& client) const = 0;   // 0 for a client never seen
    virtual bool failed() const = 0;
};

// Whether a step of `changes` to session `id` may be taken by a store that holds `had` (null: not held) and whose
// next id is `next`: refused with why otherwise. Both stores apply this one rule.
bool stepFits(SessionId id, const StoredSession* had, SessionId next, const std::vector<SessionChange>& changes,
              std::string* why);

class MemoryStore : public SessionStore {
public:
    const StoredSession* find(SessionId id) const override;
    std::size_t size() const override { return sessions_.size(); }
    std::vector<SessionId> ids() const override;
    SessionId nextId() const override { return next_; }
    bool commit(SessionId id, const std::vector<SessionChange>& changes, const std::string& client,
                std::uint64_t num, std::string* why) override;
    std::uint64_t applied(const std::string& client) const override;
    bool failed() const override { return false; }

private:
    std::map<SessionId, StoredSession> sessions_;
    std::map<std::string, std::uint64_t> applied_;
    SessionId next_ = 1;
};

// The bounds that keep every diff small (sync/sessions.h): no single text larger than `text` bytes, no queue longer
// than `queue` prompts or `queueBytes` bytes, at most `sessions` sessions, and each transcript's tail by `tail`. With
// the defaults a diff from the empty state is at most 16 sessions of (a 32 KiB closed entry or a 16 KiB tail), a 32 KiB
// open entry and a 32 KiB queue: 96 KiB a session, about 1.5 MiB in all, under the 4 MiB mosh can carry (its
// compressor's buffer, mosh src/network/compressor.h). These numbers follow from that limit and are not measured. A
// session's account, model and effort are each at most `setting` bytes of printable ASCII.
struct Limits {
    std::size_t text = 32 * 1024;
    std::size_t queue = 8;
    std::size_t queueBytes = 32 * 1024;
    std::size_t sessions = 16;
    std::size_t setting = 64;
    std::size_t friendName = 64;
    Tail tail;
};

extern const char* const kReceived;         // the placeholder reply: "(received)"
extern const char* const kDefaultHarness;   // the harness of a session made with no Responder: "claude"

// What a send, a queue, a stop and a change of a session's settings do once they are within the hub's limits.
class Responder {
public:
    virtual ~Responder() = default;
    // What a session made with `friendName` is when it copies from no other: its harness, and the friend's default
    // account, model and effort.
    virtual SessionInfo defaults(const std::string& friendName) const = 0;
    // The changes a send of `prompt` makes to session `id` (the person's entry and anything after it, or a notice that
    // refuses it), which the hub commits in one step with the client's counter.
    virtual std::vector<SessionChange> send(SessionId id, const std::string& prompt) = 0;
    // The changes a queue of `prompt` makes: the prompt at the end of the queue first, then anything that follows (the
    // oldest taken at once, when the friend is free), committed the same way.
    virtual std::vector<SessionChange> queue(SessionId id, const std::string& prompt) = 0;
    // The changes a stop of the friend's work in session `id` makes (a start cancelled: its prompt no longer waiting,
    // and a notice; none for an open turn or nothing running), committed the same way.
    virtual std::vector<SessionChange> stop(SessionId id) = 0;
    // Whether the store took the step of the send, queue or stop just given. Only once it did may the responder act
    // on it.
    virtual void sent(SessionId id, bool committed) = 0;
    // Session `id` is now `info` (its account, model or effort changed), durable: from its next turn on.
    virtual void configured(SessionId id, const SessionInfo& info) = 0;
    // Session `id` was just made, durable: what the responder shows of it from the start (its activity).
    virtual void made(SessionId id) { (void)id; }
};

// Where the accounts and friends are kept between runs: nexus-server's config repository (server/config_repo.h), the
// files accounts.json and friends.json a person or an AI edits by hand. The hub hands it the registry an action makes
// before it takes that registry as its own, with a one-line message saying what changed and who asked; refused (with
// why: the files hold hand edits not yet applied, or the disk failed), the hub keeps the registry it had.
class AccountsKeeper {
public:
    virtual ~AccountsKeeper() = default;
    virtual bool keep(const accounts::Registry& registry, const std::string& message, std::string* why) = 0;
};

class Hub {
public:
    // The state starts as every session the store already holds. Actions go to `responder` when one is given.
    explicit Hub(SessionStore& store, Limits limits = {}, Responder* responder = nullptr);

    // Applies one action (each once: the link's Applier sees to that), deciding against the sessions the store holds.
    // A send gives the responder's changes, or without one the prompt and then the placeholder reply; a queue puts the
    // prompt in the session's queue (with the responder's changes after it); a stop gives the responder's changes, or
    // none. A create makes a session with its friend, copying the account, model and effort of the session it names,
    // or the friend's defaults; an ensure does the same with the defaults, only when the friend has no session (else
    // it changes nothing, and is not refused). A set changes the session's account, model or effort.
    //
    // Refused, changing no session but counted: an action naming a session the store does not hold (nothing is made
    // for it); a create or an ensure past the limit on sessions, with a friend name empty or too long, or carrying
    // text; a create copying a session the store does not hold; a stop carrying text. Refused with a notice in the
    // session's transcript: a prompt empty or past the limits, a setting too long or not printable. Either way, with
    // `client` given, the store's counter for it becomes action.num in the same step, and `why` says why. Returns
    // false when refused, or when the store did not take the step (failed() then says whether it can take any more).
    bool apply(const Action& action, std::string* why = nullptr, const std::string& client = std::string());

    // Changes that come from the server rather than a client (a friend's reply), in one step of the store, to a session
    // it holds. False, with why, when the store did not take it.
    bool commit(SessionId id, const std::vector<SessionChange>& changes, std::string* why = nullptr);

    // The friend's text still growing in session `id`, its open entry, after the stored transcript (empty: none). In
    // memory only. A session the store does not hold has none. Within the hub's text limit: longer text is refused
    // (false). Returns true when the state changed.
    bool setOpen(SessionId id, const std::string& text);

    // What the friend of session `id` is doing now (sync/sessions.h Activity), as its Responder works it out: in memory
    // only, Idle until set. A session the store does not hold has none (false). Returns true when the state changed.
    bool setActivity(SessionId id, Activity activity);

    const Sessions& state() const { return state_; }
    const Limits& limits() const { return limits_; }
    bool failed() const { return store_.failed(); }

    // The accounts and friends (models/Accounts.tla). Until useAccounts(), the hub holds none, and a session's account
    // is any printable setting, as before accounts. From it on, every decision about a session's account is made
    // against `registry`, the one last applied (the config repository's committed files):
    //  - a create or an ensure takes the account of the session it copies when that is linked to its friend (or none),
    //    else the friend's default; a session on an account runs its provider's harness;
    //  - a set of a session's account takes one linked to its friend, or none, whatever its provider or sign-in, and is
    //    refused with a notice otherwise;
    //  - the account actions (AddAccount ... SetDefault) change the registry, kept by `keeper` first;
    //  - after every change of the registry, and at useAccounts(), the sessions are settled: each on an account no
    //    longer linked to its friend moves to the friend's default when signed in, else its first signed-in linked
    //    account, else none, and each whose account's provider changed takes its harness; each one changed is told,
    //    with a notice in its transcript, and its responder is told the session's new settings.
    // Returns the lines that say what settling did (empty: nothing).
    std::vector<std::string> useAccounts(accounts::Registry registry, AccountsKeeper* keeper);
    // The keeper alone, when the registry the server last applied could not be read: the hub holds no accounts until
    // applyAccounts() gives it some.
    void keepWith(AccountsKeeper* keeper) { keeper_ = keeper; }
    const accounts::Registry* accounts() const { return accounts_ ? &*accounts_ : nullptr; }

    // After accounts.json was first made from the drivers file (the accounts its lines passed with --account): each
    // session on no account whose friend now has a default takes it, told, so that it runs on the account it ran on
    // before. Returns what it did, a line per session. Idempotent: a session on an account is left as it is, so after a
    // store that failed part way the server runs it again at its next start (server/config_repo.h keepSeed). A
    // session's instance is settled with the rest (useAccounts): one with none takes its friend's default instance.
    std::vector<std::string> adoptDefaults();

    // A hand edit of the files (`nexus-server config reload`), or an earlier version of them (`config restore`):
    // `next` becomes the registry, kept by the keeper with `message`, and the sessions are settled. Refused whole,
    // changing nothing, when it is not consistent (accounts::problems) or the keeper refuses it (false); `report` gets
    // every problem then, else every change made, the sessions' included. Once the keeper has committed it, it is
    // applied (true) even if the store fails while the sessions move: `report` then says so, and the server, whose
    // store has failed, stops; at its next start it settles the sessions against what was committed.
    bool applyAccounts(const accounts::Registry& next, const std::string& message, std::vector<std::string>* report);

    // A sign-in (or its loss) reported for an account: the server's own state, in memory only (every account needs
    // sign-in at a start until its driver or the sign-in reports it). Ignored for an account not set up; an account
    // removed loses its sign-in, so one set up again needs sign-in again (models/Accounts.tla, Report and AddAccount).
    void reportSignedIn(const std::string& account, bool signedIn);
    const std::set<std::string>& signedIn() const { return signedIn_; }
    // What says, before any driver reports, whether an account is signed in (the server: its credential file is
    // there). Asked of each account when it first appears in the registry (at useAccounts() for every one), before the
    // sessions are settled; a driver's report afterwards overrides it. Set before useAccounts().
    void probeSignInWith(std::function<bool(const accounts::Account&)> probe) { probe_ = std::move(probe); }

    // Where a usage limit on session `id`'s account is reported: the session moves to its friend's next signed-in
    // linked account, in link order, with a notice; with none, the notice says so and the session stays. False, with
    // why, when the session is not held, the hub holds no accounts, or there was none to move to.
    bool rotate(SessionId id, std::string* why = nullptr);

private:
    bool make(const Action& action, const SessionInfo& info, std::string* why, const std::string& client);
    bool set(const Action& action, std::string* why, const std::string& client);
    bool accountAction(const Action& action, std::string* why, const std::string& client);
    bool count(const Action& action, std::string* why, const std::string& client);
    bool counted(const Action& action, const std::string& refused, std::string* why, const std::string& client);
    // `before`: the registry the hub held before this change, to find instances moved in place (null: none known).
    bool settle(std::vector<std::string>* report, const std::string& cause,
                const accounts::Registry* before = nullptr);
    void take(accounts::Registry next);
    bool moveTo(SessionId id, const SessionInfo& info, const std::string& notice);
    std::string harnessOf(const std::string& account, const std::string& otherwise) const;
    void refresh(SessionId id);
    SessionStore& store_;
    Limits limits_;
    Responder* responder_;
    std::map<SessionId, EntryRef> open_;     // each session's open entry, if it has one
    std::map<SessionId, Activity> activity_; // each session's activity, if not Idle
    Sessions state_;
    std::optional<accounts::Registry> accounts_;
    AccountsKeeper* keeper_ = nullptr;
    std::set<std::string> signedIn_;
    std::function<bool(const accounts::Account&)> probe_;
};

} // namespace nexus::sync
