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
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "accounts/registry.h"
#include "policy/host.h"
#include "sync/actions.h"
#include "sync/secrets.h"
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
    // The login of `account` just arrived (the secret dialog stored it): it counts as signed in from now, without a
    // restart, and each of its sessions held back for want of it goes on.
    virtual void signedIn(const std::string& account) { (void)account; }
    // Session `id`'s friend was just taken out of friends.json (`present` false), its notice durable, or put back
    // (true), its session settled: taken out, what runs in it is stopped and its driver ended, its prompts kept; put
    // back, they go on (models/Supervisor.tla RemoveFriend, AddBack). Also, for every session, once the first registry
    // is applied after none was held: whether its friend is in it (Learn).
    virtual void present(SessionId id, bool present) {
        (void)id;
        (void)present;
    }
};

// Where the secret dialog's effects go besides the sessions (sync/secrets.h): each asker's one answer, and the log's
// lines. The notes go into the asker's session as notices, by the hub.
class SecretEvents {
public:
    virtual ~SecretEvents() = default;
    virtual void answered(const SecretReply& reply) = 0;
    virtual void logged(const SecretLog& line) = 0;
    // Every note, whether or not its asker's session is held (the walk reads them).
    virtual void noted(const SecretNote& note) { (void)note; }
    // A line of in-app sign-in's (sync/signin.h) for server.log: never a URL, a code or a token.
    virtual void signInLogged(const std::string& line) { (void)line; }
};

// What in-app sign-in does outside the hub (server/signin_runner.h): start the account's `claude setup-token` for flow
// `id`, end it, and replay a window's callback to its listener on the server's loopback.
struct SignInHooks {
    std::function<void(std::uint64_t id, const std::string& account)> start;
    std::function<void(const std::string& account)> stop;
    std::function<void(std::uint64_t id, const std::string& account, const SignInTarget& target,
                       const SecretValue& query)> replay;
    // Whether any linked window reads sign-ins (its link of format kSignInFormat or later). Unset: taken as yes. When
    // windows that take a paste are linked but none of them reads sign-ins (each from a build before them), an
    // account is asked for by a paste instead (Hub::signInWindowsChanged).
    std::function<bool()> takers;
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
    explicit Hub(SessionStore& store, Limits limits = {}, Responder* responder = nullptr,
        std::function<std::string()> bundlePath = [] { return std::string(); });

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

    // The state every window receives: the sessions, the open secret requests and, once the hub holds a registry
    // (useAccounts(), applyAccounts()), its friends in order (Sessions::friends); with no registry, none known.
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
    //    with a notice in its transcript, and its responder is told the session's new settings;
    //  - a session whose friend is not in the registry is left as it is (its account, harness and instance), and
    //    settled like the rest once its friend is back. A change that takes its friend out tells it, in one notice,
    //    and tells the responder (Responder::present), which stops what runs in it and keeps its prompts; one that
    //    puts its friend back settles it, then tells the responder, and what waits goes on.
    // Returns the lines that say what settling did (empty: nothing).
    std::vector<std::string> useAccounts(accounts::Registry registry, AccountsKeeper* keeper);
    // The keeper alone, when the registry the server last applied could not be read: the hub holds no accounts until
    // applyAccounts() gives it some. While it holds none no friend is known to be in friends.json, so the responder
    // lists none (server/drivers.h: nothing is sent and no driver starts; prompts are kept); the first registry
    // applied then tells the responder, for every session, whether its friend is in it (Responder::present), judged
    // from that registry alone. useAccounts() tells it nothing: it comes before any driver starts, and the drivers read
    // the registry as they start.
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
    //
    // A loss (a driver's no-credential) asks the person for the login in the window only when the login hook
    // (loginsWith) says a paste can fix it, with its words as the dialog's reason: the first login, one not of a
    // token's shape, or one the provider revoked or that ended with no way to renew it. Otherwise (the file there but
    // not usable as it lies, say its mode) nothing is asked: the account's sessions are told why and the fix. Nothing
    // is asked while a request for the account is open or its value held.
    void reportSignedIn(const std::string& account, bool signedIn);
    const std::set<std::string>& signedIn() const { return signedIn_; }
    // Whether a paste would fix an account's lost login, and the words for it (server/login_life.h loginAsk).
    struct LoginAsk {
        bool ask = false;       // true: ask in the window, with `text` as the reason; false: tell the sessions `text`
        std::string text;
    };
    void loginsWith(std::function<LoginAsk(const accounts::Account&)> hook) { loginAsk_ = std::move(hook); }
    // An account's login has ended and cannot be renewed (server/login_life.h LoginWatch): it is signed out, and nexus
    // asks for a new one in the window with `why`, unless a request for it is open or held.
    void needsSignIn(const std::string& account, const std::string& why);
    // A notice, `text`, in every session on `account` (its login's end coming, say), and in server.log.
    void tellAccount(const std::string& account, const std::string& text);
    // What says, before any driver reports, whether an account is signed in (the server: its credential file is
    // there). Asked of each account when it first appears in the registry (at useAccounts() for every one), before the
    // sessions are settled; a driver's report afterwards overrides it. Set before useAccounts().
    void probeSignInWith(std::function<bool(const accounts::Account&)> probe) { probe_ = std::move(probe); }

    // Where a usage limit on session `id`'s account is reported: the session moves to its friend's next signed-in
    // linked account, in link order, with a notice; with none, the notice says so and the session stays. False, with
    // why, when the session is not held, the hub holds no accounts, or there was none to move to.
    // Pure request handlers. The server commits the returned text with the request's acknowledgement.
    using NativeService = std::function<std::string(const std::string&)>;
    void registerService(std::string name, NativeService handler) { services_[std::move(name)] = std::move(handler); }
    bool rotate(SessionId id, std::string* why = nullptr);

    // The secret dialog (sync/secrets.h, docs/secrets.md, models/Secrets.tla). Until secretsWith() the hub takes no
    // asks and every secret answer or cancel is counted and dropped. `keeper` writes a value to its account's file,
    // `events` takes the askers' answers and the log's lines, `takers` says whether any connected window can take a
    // request (a window of format 3 or later), and `clock` is now, in milliseconds, for the deadlines.
    void secretsWith(SecretKeeper* keeper, SecretEvents* events, std::function<bool()> takers,
                     std::function<std::uint64_t()> clock);
    // An ask for `what` of `account`, from `asker` with its one-line reason `why`. Malformed, with no request made and
    // no id minted (false, with `refused` saying why): `account` or `what` is not a name, the hub holds no accounts or
    // none named `account`, the asker's friend is not linked to it (nexus itself, session 0, asks for any), its
    // provider documents no file `what`, or `why` is not a safe line (isSecretWhy). An asker with an ask open already,
    // or kSecretAsksPerWindow asks in the last kSecretAskWindowMs, is answered busy at once, with no request made and
    // no id minted (the answer's id is 0). Otherwise an id is minted (durable: the store's counter under
    // kSecretIdsClient, so never reused across restarts), and the request opens, or is refused at once (busy,
    // no-client) with its answer to the asker. False, with why, when the store did not take the id.
    bool askSecret(const SecretAsker& asker, const std::string& account, const std::string& what,
                   const std::string& why, std::string* refused = nullptr);
    // Every open request past its deadline is refused (timeout).
    void expireSecrets();
    // When expireSecrets() next has something to do (by the clock given to secretsWith), or none.
    std::optional<std::uint64_t> secretDeadline() const { return desk_.deadline(); }
    // The oldest held value whose time has come is written again (SecretDesk::retry).
    void retrySecrets();
    // When retrySecrets() next has something to do, or none.
    std::optional<std::uint64_t> secretRetryAt() const { return desk_.retryAt(); }
    // When the server's loop must next call expireSecrets() and retrySecrets(): the sooner of the two, or none.
    std::optional<std::uint64_t> secretWake() const;
    // nexus's own ask whose paste was refused malformed is asked again, one per call, unless a request for the account
    // is open or held by then: the account asked for, or none.
    std::optional<std::string> reaskSecrets();
    bool reaskPending() const { return !reask_.empty(); }
    // The server is ending: every open request is dropped unanswered, each with its log line.
    void dropSecrets();
    const SecretDesk& secrets() const { return desk_; }
    // The walk's faulty run alone: SecretDesk::injectEchoFault.
    void injectSecretEchoFault() { desk_.injectEchoFault(); }

    // In-app sign-in (sync/signin.h, docs/secrets.md "Sign-in", models/SignIn.tla). Once signInWith() is given (after
    // secretsWith()), an account of a provider with in-app sign-in (hasSignIn) that has no login is signed in in the
    // browser instead of by a paste: at the start for each with no login (probeSignInWith), at a driver's no-credential,
    // at a login's end, and at an AI's ask (requestSignIn). `host` is the server's host name, for the windows' check
    // of whether they run on its machine.
    void signInWith(SignInHooks hooks, std::string host);
    bool signsIn(const std::string& account) const;
    // An AI's `config sign-in <account>`: what it is told, one line, never anything of the flow: "signed in", or
    // "needs sign-in: ...", which starts a flow when none is live; false with why when it cannot be signed in so.
    bool requestSignIn(const std::string& account, std::string* said);
    // The account's process handed over its URL; printed its token; ended with no token (`why`, one line).
    void signInUrl(const std::string& account, std::uint64_t id, const SecretValue& url);
    void signInPrinted(const std::string& account, const SecretValue& token);
    void signInExited(const std::string& account, std::uint64_t id, const std::string& why);
    // A window's link ended: the flows it held (by its claim token) are let go of.
    void signInWindowGone(const std::string& opener);
    // A window linked or its link ended. When the linked windows take a paste but none reads sign-ins (an older
    // window, a newer server), every needed flow ends and its account is asked for its login by a paste, as before
    // sign-ins (SignInDesk::fallBack); a flow a window holds already goes on.
    void signInWindowsChanged();
    // Every running flow past its deadline expires; when that is next due.
    void expireSignIns();
    std::optional<std::uint64_t> signInDeadline() const { return signIns_.deadline(); }
    // The server is ending: every flow is dropped and its process ended.
    void dropSignIns();
    // The state for the link of the window whose claim token is `opener`: state() with the URL of each flow that
    // window holds. state() itself carries no URL.
    Sessions stateFor(const std::string& opener) const;
    const SignInDesk& signIns() const { return signIns_; }
    // The walk's faulty runs alone: SignInDesk's seams.
    void injectSignInClaimFault() { signIns_.injectClaimFault(); }
    void injectSignInCallbackFault() { signIns_.injectCallbackFault(); }

private:
    std::vector<SignInOffer> publicOffers() const;
    void needSignIn(const std::string& account);
    void needAll();
    // Windows that take a paste are linked, and none reads sign-ins: a paste is asked for instead of a sign-in.
    bool pasteInstead() const;
    void askPasteLogin(const std::string& account);
    void noteAccount(const std::string& account, const std::string& text);  // a notice in each session on it
    void signInEffects(const SignInEffects& effects);
    bool signInAction(const Action& action, std::string* why, const std::string& client);
    void secretEffects(const SecretEffects& effects);
    bool secretAction(const Action& action, std::string* why, const std::string& client);
    bool make(const Action& action, const SessionInfo& info, std::string* why, const std::string& client);
    bool applyMessage(const Action& input, const StoredSession& session, std::string* why, const std::string& client);
    bool set(const Action& action, std::string* why, const std::string& client);
    bool accountAction(const Action& action, std::string* why, const std::string& client);
    bool count(const Action& action, std::string* why, const std::string& client);
    bool counted(const Action& action, const std::string& refused, std::string* why, const std::string& client);
    // `before`: the registry the hub held before this change, to find instances moved in place (null: none known).
    // A friend in `before` and not in the registry now was taken out by this change; one not in it was put back.
    bool settle(std::vector<std::string>* report, const std::string& cause,
                const accounts::Registry* before = nullptr);
    bool settleOne(SessionId id, std::vector<std::string>* report, const std::string& cause,
                   const accounts::Registry* before);
    bool removed(SessionId id, const std::string& friendName, const std::string& cause,
                 std::vector<std::string>* report);
    void take(accounts::Registry next);
    void stateFriends();    // the registry's friends into the state (Sessions::setFriends), when they changed
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
    std::map<std::string, NativeService> services_;
    policy::Host application_{"server"};
    std::function<std::string()> bundlePath_;
    std::set<std::string> signedIn_;
    std::function<bool(const accounts::Account&)> probe_;
    SecretDesk desk_;
    SecretKeeper* secretKeeper_ = nullptr;
    SecretEvents* secretEvents_ = nullptr;
    std::function<bool()> takers_;
    std::function<std::uint64_t()> clock_;
    std::map<SessionId, std::deque<std::uint64_t>> askTimes_;   // each asker's asks in the last kSecretAskWindowMs
    std::function<LoginAsk(const accounts::Account&)> loginAsk_;
    std::deque<std::pair<std::string, std::string>> reask_;     // nexus's own asks to make again: account, what
    SignInDesk signIns_;
    std::optional<SignInHooks> signInHooks_;
};

} // namespace nexus::sync
