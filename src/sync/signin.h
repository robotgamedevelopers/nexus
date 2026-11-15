// In-app sign-in for the Claude harness (docs/secrets.md, "Sign-in"; models/SignIn.tla is the model, and
// tests/sync/signin_walk.cpp binds this code to it). An account with no login is signed in from inside nexus with one
// browser sign-in: no paste, no terminal, no file to save, and no AI ever sees the sign-in URL, the code the browser
// carries back or the token.
//
// The server marks an account needed (its driver said no credential, no token is stored, or an AI asked with
// `nexus-server config sign-in`): a flow, its id durable and never reused, offered in the state every window receives
// (SignInOffer). A window the person has just shown claims the oldest offer it has not claimed (SignInOpener). The
// first claim the server takes for a needed flow starts the unmodified `claude setup-token` for the account (server/
// signin_runner.h), with nexus's helper as its `open` and BROWSER, which hands the sign-in URL to the server and opens
// nothing there. The server offers the URL to the claiming window alone; it opens it in the system browser. On the
// server's own machine Claude Code's listener takes the browser's redirect itself; on another machine the window binds
// 127.0.0.1 on the redirect's port and takes exactly one GET on its path whose state matches (CallbackGate), answers the
// browser, and sends the query up once; the server replays it once to the listener on its own loopback. The process
// prints the token, which the server takes from its terminal (TokenScanner) straight into the one-chance store
// (SecretDesk::capture): never dropped once captured. The account is signed in and its drivers start.
//
// Where the URL, the code and the token are: the URL in the server's memory (SignInDesk), the state's offer to the
// claiming window, and the window's browser; the code in the browser's redirect, the window's gate and its one action
// up, and the server's replay; the token in the process's terminal, the scanner, and the one-chance store. Each is a
// SecretValue, which has no way to print itself: none is ever in a transcript, a log line, a driver's input or a state
// file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "sync/secrets.h"

namespace nexus::sync {

// A flow's deadline: 10 minutes from its process's start. A guess, not a measurement: long enough to sign in, two-step
// check included; short enough that a page left unfinished does not hold the account's process for long.
constexpr std::uint64_t kSignInDeadlineMs = 10 * 60 * 1000;
// The client name under which the store keeps the last flow id minted (SessionStore::applied), as kSecretIdsClient
// keeps the secret requests': no window can take it, so an id is never reused across restarts.
extern const char* const kSignInIdsClient;
// The longest URL, query and claim token taken, in bytes. A judgement, not measured: an OAuth URL with a PKCE challenge
// and a state is well under 1 KiB, its callback's query smaller; these leave room and keep an action small.
constexpr std::size_t kSignInUrlBytes = 4096;
constexpr std::size_t kSignInQueryBytes = 4096;
constexpr std::size_t kSignInTokenBytes = 64;
// What the state says a flow's opener is to every window but the one holding it: held, by a window it does not name.
// Not a claim token (isClaimToken), so no window's token is ever equal to it, and no window learns another's token.
constexpr const char* kSignInHeldBy = "*";

// Whether accounts of `provider` sign in in the app: claude (`claude setup-token`).
bool hasSignIn(const std::string& provider);

// Where a sign-in URL sends the browser back: its redirect_uri, which must be http on localhost or 127.0.0.1, with a
// port; and the state the URL carries, which the callback must carry back.
struct SignInTarget {
    std::uint16_t port = 0;
    std::string path;           // the redirect's path, such as /callback
    std::string state;          // the URL's state
    bool operator==(const SignInTarget&) const = default;
};
// Whether `host` is one of the hosts `provider`'s sign-in sends the browser to: one list per harness (claude:
// claude.com, platform.claude.com, claude.ai), compared byte for byte.
bool signInHostAllowed(std::string_view provider, std::string_view host);
// An https URL's authority: the bytes between "https://" and the first '/', '?' or '#' (userinfo and port included, so
// that a URL with either is on no list); empty for any other URL.
std::string_view urlHost(std::string_view url);
// The URL's redirect and state; false when it is not an https URL to one of `provider`'s sign-in hosts with both, or
// holds a space or a control byte (the process's URL is then refused and its flow fails; a window opens nothing).
bool parseSignInUrl(std::string_view provider, std::string_view url, SignInTarget* out);
// Whether `a` and `b` hold the same bytes, in a time that depends on their lengths alone: for a state and a nonce.
bool sameBytes(std::string_view a, std::string_view b);
// Whether `query` holds only bytes a request line carries as they are: no space, CR, LF or other control byte.
bool isQueryBytes(std::string_view query);
// The value of `name` in a URL's query (the part after '?', without it), percent-decoded; empty when absent.
std::string queryValue(std::string_view query, std::string_view name);
// Whether `token` may name a window's claim: 1 to kSignInTokenBytes letters and digits.
bool isClaimToken(std::string_view token);
// Whether a token printed by `claude setup-token` is a subscription login (an OAuth token), not an API key.
bool isSubscriptionToken(const SecretValue& token);
// Whether the window and the server run on one machine, so that Claude Code's own listener takes the redirect and the
// window binds nothing: the window reaches the server at a loopback address, or the server's host name (`host`, from
// the offer) is the window's own (`local`), compared by their first label, case aside.
bool sameMachine(std::string_view serverArg, std::string_view host, std::string_view local);

// A flow as every window of format 5 or later receives it, after the secret requests in the state (schema's
// SignInOffer, docs/formats.md). The URL is there only
// for the window that holds the flow (opener), and only once the process has handed it over.
struct SignInOffer {
    std::uint64_t id = 0;
    std::string account;
    std::string provider;
    std::string opener;         // the claim token of the window that holds it; empty: none
    std::string host;           // the server's host name, for sameMachine
    SecretValue url{};          // with an opener, once the process gave it; else empty
    bool operator==(const SignInOffer&) const = default;
};

// A flow's state (models/SignIn.tla): needed (no process; offered for a claim); started (its process runs, no URL
// yet); url-sent (the URL offered to the window that holds it); waiting (that window opened it); callback-forwarded
// (the window's callback replayed to the listener); and how it ended: captured (the token went to the one-chance
// store), failed, cancelled (the person declined on the page), expired (past its deadline), dropped (the server ended,
// or the account was signed in otherwise).
enum class SignInState : std::uint8_t {
    Needed, Started, UrlSent, Waiting, CallbackForwarded, Captured, Failed, Cancelled, Expired, Dropped
};
const char* signInStateName(SignInState state);
bool signInRunning(SignInState state);  // started .. callback-forwarded: its process runs

// What one step of the desk did. Never a URL, a code or a token, but in `replays` (the query, a SecretValue).
struct SignInEffects {
    struct Start {
        std::uint64_t id = 0;
        std::string account;
    };
    struct Replay {
        std::uint64_t id = 0;
        std::string account;
        SignInTarget target;
        SecretValue query;
    };
    struct Note {
        std::string account;
        std::string text;
    };
    std::vector<Start> start;               // the processes to start now
    std::vector<std::string> stop;          // the accounts whose process ends now
    std::vector<Replay> replays;            // the callbacks to replay to their listener
    std::vector<Note> notes;                // a notice in each session on the account
    std::vector<std::string> log;           // server.log's lines
    std::vector<std::string> needAgain;     // accounts whose flow ended unsigned: each needs a fresh flow
    bool changed = false;                   // the offers changed: the windows' state must be sent again
};

// The server's side: one flow per account at most, each started by the first claim the desk takes for it. Pure: the
// time is an argument, the caller mints each id durably and does what the effects say.
class SignInDesk {
public:
    void setHost(std::string host) { host_ = std::move(host); }

    // Whether `account` has a live flow (needed or running).
    bool live(const std::string& account) const;
    // The live flow of `account`: its id and state.
    std::optional<std::pair<std::uint64_t, SignInState>> flowOf(const std::string& account) const;
    // How flow `id` ended, or its state while live; none for one this desk never saw (or long ended).
    std::optional<SignInState> stateOf(std::uint64_t id) const;

    // `account` (of `provider`) has no login: flow `id` (minted, one past the last) is needed, unless one is live.
    SignInEffects need(std::uint64_t id, const std::string& account, const std::string& provider);
    // A window's claim, by its claim token. A needed flow starts (its process, due at now + kSignInDeadlineMs); a
    // running one that no window holds is taken over, and nothing starts; any other claim is late.
    SignInEffects claim(std::uint64_t id, const std::string& opener, std::uint64_t now);
    // The process of flow `id` handed over its URL: refused (the flow fails) unless it parses (parseSignInUrl).
    SignInEffects url(const std::string& account, std::uint64_t id, const SecretValue& url);
    // The window holding flow `id` opened its URL.
    SignInEffects opened(std::uint64_t id, const std::string& opener);
    // The window's gate sent a callback: replayed once, from the window holding the flow, while it waits, and only
    // with the state of its URL and a code or an error; any other is dropped, its bytes in no effect.
    SignInEffects callback(std::uint64_t id, const std::string& opener, const SecretValue& query);
    // The process of `account` printed its token: the flow is captured (the caller puts the token in the one-chance
    // store). Returns the flow's id; 0 when none was running, and then it is late().
    std::uint64_t printed(const std::string& account, SignInEffects* out);
    // The process of `account` printed its token after its flow ended (the deadline passed while the browser was sent
    // back, or in the grace after its process was told to end): the caller stores it all the same, and the account is
    // signed in. A needed flow for the account ends, dropped. models/SignIn.tla Print, its late branch.
    void late(const std::string& account, SignInEffects* out);
    // The process of `account` ended with no token: its flow fails, or is cancelled when the person declined.
    SignInEffects exited(const std::string& account, std::uint64_t id, const std::string& why);
    // Every running flow past its deadline expires, its process ended.
    SignInEffects expire(std::uint64_t now);
    // A window's link ended: the flows it held are let go of, for another window (or it, back) to take over.
    SignInEffects windowGone(const std::string& opener);
    // `account` was signed in otherwise (a paste, its driver): its flow ends, dropped, its process ended.
    SignInEffects signedIn(const std::string& account);
    // No linked window can sign an account in in the browser (each is of a format before sign-ins), so the hub asks
    // for a paste instead: every needed flow (no process, no window holding it) ends, dropped, and its account is
    // returned. models/SignIn.tla FallBack.
    std::vector<std::string> fallBack(SignInEffects* out);
    // The server is ending: every live flow is dropped and every process ended.
    SignInEffects drop();

    // The live flows, as every window receives them.
    std::vector<SignInOffer> offers() const;
    // When expire() next has something to do.
    std::optional<std::uint64_t> deadline() const;

    static constexpr std::size_t kKeptEnded = 256;

    // Test seams for the trace walk's faulty runs alone (tests/sync/signin_walk): a claim of any flow no window holds
    // starts a process (a second one, for a running flow); a callback is replayed whatever its state. nexus-server never
    // calls them.
    void injectClaimFault() { claimFault_ = true; }
    void injectCallbackFault() { callbackFault_ = true; }

private:
    struct Flow {
        std::uint64_t id = 0;
        std::string account;
        std::string provider;
        SignInState state = SignInState::Needed;
        std::string opener;
        SecretValue url;
        SignInTarget target;
        std::uint64_t due = 0;
        bool denied = false;    // the callback replayed carried the person's refusal
    };
    Flow* byId(std::uint64_t id);
    void end(const std::string& account, SignInState state, SignInEffects& out);
    std::map<std::string, Flow> flows_;             // the live flows, by account
    std::map<std::uint64_t, SignInState> ended_;    // the most recent flows ended, by id
    std::string host_;
    bool claimFault_ = false;
    bool callbackFault_ = false;
};

// The window's side: which offer it claims and which URL it opens. A show of the overlay (the person, there) arms one
// claim: of the oldest offer no window holds that this window has not claimed before. An offer this window holds, with
// its URL, is opened once: in the system browser, and with a gate first when the server is on another machine; a page
// that needs a gate waits while another flow's gate waits for its callback (one gate per window). A URL to a host not
// on its provider's list is never opened. A link lost closes the gate; what was claimed and opened is kept, so nothing
// is claimed or opened twice.
class SignInOpener {
public:
    explicit SignInOpener(std::string token = std::string()) : token_(std::move(token)) {}
    const std::string& token() const { return token_; }
    void setToken(std::string token) { token_ = std::move(token); }

    void received(const std::vector<SignInOffer>& offers);
    void lost();
    void shown() { armed_ = true; }
    void hidden() { armed_ = false; }
    bool armed() const { return armed_; }

    struct Step {
        // Refused: the offer's URL is not a sign-in page of its provider's (parseSignInUrl); nothing is opened.
        enum class Kind : std::uint8_t { None, Claim, Open, Refused } kind = Kind::None;
        std::uint64_t id = 0;
        std::string account;
        SecretValue url;            // with Open
        bool gate = false;          // with Open: bind the gate first (the server is on another machine)
        SignInTarget target;        // with gate: where the gate listens and what it takes
    };
    // The next thing to do, one at a time: claim (an action up), or open (the gate, then the browser, then an
    // action up). `same` says whether an offer's server is on this machine (sameMachine).
    Step next(bool (*same)(const SignInOffer&, const void*), const void* context);
    // The flow the gate is bound for (0: none): it closes when that flow leaves the state or the link is lost.
    std::uint64_t gate() const { return gate_; }
    void gateClosed() { gate_ = 0; }
    const std::vector<SignInOffer>& view() const { return view_; }
    const std::set<std::uint64_t>& claimed() const { return claimed_; }
    const std::set<std::uint64_t>& opened() const { return opened_; }

private:
    std::string token_;
    std::vector<SignInOffer> view_;
    std::set<std::uint64_t> claimed_;
    std::set<std::uint64_t> opened_;
    std::uint64_t gate_ = 0;
    bool armed_ = false;
};

// The window's one-shot gate, as pure logic over one connection's bytes: a GET on the target's path whose state
// matches, carrying a code or an error, is taken once (its query, to send up); anything else is answered "not found"
// and changes nothing, since a wrong request to Claude Code's listener would end the whole login. After the one, every
// request is refused.
class CallbackGate {
public:
    explicit CallbackGate(SignInTarget target) : target_(std::move(target)) {}
    enum class Verdict : std::uint8_t { More, Refuse, Take };
    // The bytes one connection sent so far. More: the request line and headers are not all there yet (at most
    // kRequestBytes are read). Refuse and Take come with `response`, the bytes to answer with; Take with `query`.
    Verdict take(std::string_view bytes, SecretValue* query, std::string* response);
    bool taken() const { return taken_; }
    static constexpr std::size_t kRequestBytes = 8192;

    // A test seam for the trace walk's faulty run alone: the gate takes any GET, whatever its path or state.
    void injectStrayFault() { strayFault_ = true; }

private:
    SignInTarget target_;
    bool taken_ = false;
    bool strayFault_ = false;
};

// The window's gate's connections, as pure logic (signin_gate.h serves them): at most kConnections at once; each has
// kConnectionMs from its accept, whatever it sends meanwhile, so that a peer trickling bytes holds a slot no longer
// than one sending nothing; one past the cap is refused. The time is an argument.
class GateSlots {
public:
    static constexpr std::size_t kConnections = 8;
    // A guess, not measured: a browser sends its redirect's few hundred bytes at once.
    static constexpr std::uint64_t kConnectionMs = 10000;
    // A connection accepted at `now`: true with the slot it holds; false when every slot is in use (it is refused).
    bool accept(std::uint64_t now, std::size_t* slot);
    // The slots whose time ran out by `now`: freed, for the caller to close.
    std::vector<std::size_t> expire(std::uint64_t now);
    void release(std::size_t slot);
    std::size_t used() const;

private:
    std::uint64_t due_[kConnections] = {};
    bool used_[kConnections] = {};
};

// Finds the sign-in URL `claude setup-token` also prints in its terminal, a few seconds after it hands it to `open`:
// the server's fallback where the program opens a browser by a path of its own rather than the helper on its PATH. A
// URL is taken only whole (ended by a space or a line's end) and only when parseSignInUrl accepts it. Holds no more of
// the output than one URL's bytes, wiped as they go.
class UrlScanner {
public:
    explicit UrlScanner(std::string provider) : provider_(std::move(provider)) {}
    bool feed(std::string_view bytes);
    bool found() const { return found_; }
    SecretValue take();
    ~UrlScanner();

private:
    std::string provider_;
    std::string text_;
    SecretValue url_;
    bool found_ = false;
    int escape_ = 0;
};

// Finds the token `claude setup-token` prints in what its terminal shows, escape sequences aside. Holds no more of the
// output than the few bytes that may start a token cut between two reads, wiped as they go.
class TokenScanner {
public:
    // More of the terminal's output; true once the token is found (take() gives it).
    bool feed(std::string_view bytes);
    bool found() const { return found_; }
    SecretValue take();
    ~TokenScanner();

private:
    std::string text_;          // the output since the last cut, escape sequences out
    SecretValue token_;
    bool found_ = false;
    int escape_ = 0;            // 0 none; 1 after ESC; 2 in CSI; 3 in OSC; 4 in OSC after ESC
};

} // namespace nexus::sync
