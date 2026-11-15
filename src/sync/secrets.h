// The secret dialog (docs/secrets.md): how a secret for an account (an API key, a token) gets from a person's fingers
// to its 0600 file on nexus-server, with no AI ever seeing it. models/Secrets.tla is the model; tests/sync/
// secrets_walk.cpp binds this code to it.
//
// An asker (a friend's AI through its driver, or the server itself) asks for a secret for an account. The server mints
// a request id (durable: never reused across restarts) and puts the request in the state every window receives: its
// id, its account, what is asked, who asks and why, never a value (Sessions::requests). A window that can take requests
// shows a dialog for the oldest open one it has not answered (SecretPrompt). The person pastes the value once into one
// window: it goes up that window's actions to the server alone (ActionKind::SecretAnswer), or Escape cancels it for
// every window (SecretCancel). The server takes the first answer for an open request (SecretDesk): it writes the value
// to the account's file (SecretKeeper) and answers the asker "stored" once it is durable, holding it and writing it
// again until then; a cancel, a deadline, a paste not of the file's shape or a refusal at the ask answers it "refused"
// with a reason. An answer for a request no longer open is dropped and logged without its value, unless it crossed the
// deadline of the newest request of its account, which is still stored.
//
// Where a value may be: a window's open dialog (SecretPrompt::typed), the window's action in flight to the server, the
// server's memory while its file cannot yet be written (SecretDesk's held values), and the account's file. Never the
// store, a transcript, a log line, a driver's input or another window's state: replies, notes and log lines are built
// from the request alone, and a SecretValue has no way to print itself.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::sync {

using SessionId = std::uint64_t;

// A secret's bytes, wiped when freed (a copy is wiped when it is freed too): the one type a value is ever held in. It
// has no stream output and no conversion to text: the only way to its bytes is bytes(), which the server's file writer
// and the action's encoder call, and nothing else.
class SecretValue {
public:
    SecretValue() = default;
    explicit SecretValue(std::string_view bytes) : bytes_(bytes) {}
    SecretValue(const SecretValue& other) : bytes_(other.bytes_) {}
    SecretValue(SecretValue&& other) noexcept;
    SecretValue& operator=(const SecretValue& other);
    SecretValue& operator=(SecretValue&& other) noexcept;
    ~SecretValue();

    const std::string& bytes() const { return bytes_; }
    bool empty() const { return bytes_.empty(); }
    std::size_t size() const { return bytes_.size(); }
    void clear();   // overwrites the bytes, then empties
    bool operator==(const SecretValue& x) const { return bytes_ == x.bytes_; }

private:
    std::string bytes_;
};

// Overwrites every byte `s` holds, its spare capacity included, then empties it: for a buffer a value passed through
// (an action's frame on the wire).
void wipeBytes(std::string& s);

// The deadline of a request: 10 minutes from the ask. A guess, not a measurement (docs/secrets.md): long enough to find a
// key in a provider's console, short enough that a forgotten request does not hold an asker's turn open for long.
constexpr std::uint64_t kSecretDeadlineMs = 10 * 60 * 1000;
// The longest reason an asker gives, in bytes: one line, shown in the dialog as quoted text.
constexpr std::size_t kSecretWhyBytes = 200;
// How many asks one asker (a session's driver) makes in a window of time before the next is refused busy at once: 5 a
// minute. A guess, not a measurement: an AI that needs a login asks once and waits for the person; more than a few a
// minute is a loop, or an AI fishing, and each ask pops a dialog in front of the person.
constexpr std::size_t kSecretAsksPerWindow = 5;
constexpr std::uint64_t kSecretAskWindowMs = 60 * 1000;
// Whether `why` may be an ask's reason, shown in the dialog and the asker's session: valid UTF-8 of at most
// kSecretWhyBytes, with no control character (C0, DEL, C1), no bidirectional control (U+061C, U+200E, U+200F,
// U+202A to U+202E, U+2066 to U+2069), no zero-width character (U+200B to U+200D, U+2060, U+FEFF) and no line or
// paragraph separator (U+2028, U+2029): one line that reads as it is.
bool isSecretWhy(std::string_view why);
// The longest value a window sends, in bytes. A judgement, not measured: tokens and API keys are under 1 KiB; 16 KiB
// holds any a provider issues with room to spare, and keeps an action far within what mosh carries.
constexpr std::size_t kSecretValueBytes = 16 * 1024;

// The client name under which the store keeps the next request id (SessionStore::applied): a name no window can take
// (validClientId refuses ':'), so a request id is durable without a change to the store's format.
extern const char* const kSecretIdsClient;

// The files an account's provider documents, which an asker may ask for: a claude account's login is its OAuth token,
// `oauth-token` (drivers/claude/src/account.ts). Empty for a provider that documents none.
std::vector<std::string> secretFiles(const std::string& provider);
// The file whose arrival signs an account of `provider` in (its login), or empty.
std::string loginFile(const std::string& provider);

// A request as every window that can take one receives it: never a value.
struct SecretRequest {
    std::uint64_t id = 0;
    std::string account;
    std::string what;           // the file asked for, one of secretFiles(provider)
    SessionId asker = 0;        // the asker's session; 0: nexus itself
    std::string askerName;      // the asker's friend, as the dialog names it; empty for nexus itself
    std::string why;            // the asker's one-line reason, at most kSecretWhyBytes
    bool operator==(const SecretRequest&) const = default;
};

// Who asks: a session's driver, by the number it gave its ask; or nexus itself (session 0).
struct SecretAsker {
    SessionId session = 0;
    std::uint64_t ask = 0;
    std::string name;           // the friend, for the dialog; empty for nexus itself
    bool operator==(const SecretAsker&) const = default;
};

// The reasons an asker is refused (docs/secrets.md, "What may end a request"): one open or held already for the account
// (or for the asker, or it asked too often); no window can take one; the person cancelled; nobody answered before the
// deadline; what was pasted is not the shape of the file asked for (secretShape). None of them is ever a failure of
// nexus's own: a value that arrived in the right shape is never refused. When its file cannot be written, the server
// holds it and writes it again until it is durable (SecretDesk::retry), and the asker hears "stored" only then.
// `unwritable` and `unconfirmed` were a server's from before that (a write failure refused, and a rename whose
// directory's sync failed answered stored); no server sends them now, and a driver still reads them.
namespace reason {
constexpr const char* kBusy = "busy";
constexpr const char* kNoClient = "no-client";
constexpr const char* kCancelled = "cancelled";
constexpr const char* kTimeout = "timeout";
constexpr const char* kMalformed = "malformed";
} // namespace reason

// Whether `value` has the shape of an account's file `what`: empty if so, else what is wrong with it, in words for
// the person (never the value, its length or any of its bytes). Every file a provider documents today (secretFiles) is
// one token: 1 to kSecretValueBytes bytes, each a printable ASCII character other than a space, so no line break, tab
// or space inside it (the dialog has already cut those at its ends). A paste of a sentence, of two lines or of nothing
// but spaces is refused at once, "malformed", and asked again, rather than stored and refused later by the harness.
std::string secretShape(const std::string& what, const SecretValue& value);

// How often a held value is written again: first 1 s after the write that failed, then twice as long each time, at most
// a minute. A guess, not a measurement: soon enough that a fixed disk is used within a minute, rare enough that a disk
// that stays broken costs a write and a log line a minute.
constexpr std::uint64_t kSecretRetryFirstMs = 1000;
constexpr std::uint64_t kSecretRetryMaxMs = 60 * 1000;

// The one answer an asker hears for a request: stored, or refused with a reason. Never the value, its length, or which
// window answered. Id 0: an ask refused before a request was made (Hub::askSecret).
struct SecretReply {
    std::uint64_t id = 0;
    SecretAsker asker;
    bool stored = false;
    std::string reason;         // with refused; empty with stored
    std::string account;        // the request's account and what was asked (empty for id 0): for the hub alone,
    std::string what;           // never on the driver's line
};

// A line in the asker's session (what its AI reads back): "asked", "stored" or "refused", with the account and the
// request's id, and its text.
struct SecretNote {
    std::uint64_t id = 0;
    SessionId asker = 0;
    std::string what;
    std::string account;
    std::string text;
};

// A line of server.log: the request id, the account, what happened (one word: "asked", "stored", "holding", "late",
// "lost", "dropped", or a refusal's reason) and, for some, more words (the filesystem's error for "holding"). Never a
// value.
struct SecretLog {
    std::uint64_t id = 0;
    std::string account;
    std::string event;
    std::string detail;
    std::string line() const;
};

// What one step of the desk did: the answers to askers, the session lines, the log lines, and the requests whose value
// became durable in this step (the hub signs an account in when it was its login).
struct SecretEffects {
    std::vector<SecretReply> replies;
    std::vector<SecretNote> notes;
    std::vector<SecretLog> log;
    std::vector<SecretRequest> stored;
    bool changed = false;       // the open requests changed: the windows' state must be sent again
};

// How a keep went: the value is in its file and durable; in its file, but its durability unconfirmed (the rename was
// made and the directory's sync failed); or not in its file (the old one, if any, is as it was). Only Durable ends a
// held value: the desk keeps the other two and writes them again (SecretDesk::retry).
enum class Kept : std::uint8_t { Durable, Unconfirmed, Failed };

// Writes a value to `accounts/<account>/<what>`: the directory made 0700 if missing, the value written to a temporary
// 0600 file beside it, synced and renamed over the old one, and the directory synced. With why (never holding the
// value) when it is not Durable.
class SecretKeeper {
public:
    virtual ~SecretKeeper() = default;
    virtual Kept keep(const std::string& account, const std::string& what, const SecretValue& value,
                      std::string* why) = 0;
};

// The state a request is in: open (in the windows' state, waiting for its answer); holding (its value arrived and the
// server holds it, in memory alone, until its file is durable); stored; refused; expired (refused, timeout, but still
// the newest request for its account, so an answer that crossed the deadline is still stored); or dropped (open or
// holding when the server ended).
enum class SecretState : std::uint8_t { Open, Stored, Refused, Dropped, Holding, Expired };

// The server's side: the open requests, each answered once, and the values held until they are durable. Pure: the
// time is an argument, the file goes through a SecretKeeper, and every effect is returned. The caller mints each id
// (Hub::askSecret, durably) before the ask.
//
// No answer loses a value that arrived in the right shape while the server runs (models/Secrets.tla
// ReceivedNeverDropped): one for an open request, or for the newest request of its account that expired, is written
// at once; if the write is not durable the value is held (Holding) and written again, with backoff, until it is. The
// asker hears "stored" only once it is durable (StoredOnlyDurable). Only the server's end loses a held value ("lost",
// logged); docs/secrets.md says so.
class SecretDesk {
public:
    // The open requests, oldest first: what every window that can take one receives.
    const std::vector<SecretRequest>& open() const { return open_; }
    // The requests whose value is held, oldest first: in no window's state (the person's part is done).
    std::vector<SecretRequest> holding() const;
    // Whether a request for `account` is open or holding: a new one for it is busy.
    bool busy(const std::string& account) const;
    // Whether session `asker` has a request open or holding: its asker still waits for the answer.
    bool waiting(SessionId asker) const;

    // A request `id` (minted, one past the last) for `account`'s `what`, from `asker`, with its reason. Refused at once,
    // answering the asker: one is already open or holding for the account (busy), or `takers` is false (no-client).
    // Else it is open until its answer, a cancel, or now + kSecretDeadlineMs.
    SecretEffects ask(std::uint64_t id, const SecretAsker& asker, const std::string& account, const std::string& what,
                      const std::string& why, bool takers, std::uint64_t now);
    // An ask refused before any request is made (Hub::askSecret: the asker has one open, or asked too often): busy,
    // with `why` in its session line and log line, and id 0.
    SecretEffects refuseAtOnce(const SecretAsker& asker, const std::string& account, const std::string& what,
                               const std::string& why) const;
    // A window's answer, at `now`. For an open request, or the newest of its account that expired: a value not of the
    // file's shape (secretShape) is refused "malformed" (an expired one's is only logged: its asker was answered);
    // otherwise it is written, and stored when durable, else held for retry() (no answer to the asker yet). For any
    // other request the answer is dropped and logged "late". The value is never put in any effect.
    SecretEffects answer(std::uint64_t id, const SecretValue& value, SecretKeeper& keeper, std::uint64_t now);
    // A window's cancel (Escape): an open request is refused, cancelled; any other is dropped and logged "late".
    SecretEffects cancel(std::uint64_t id);
    // Every open request whose deadline is at or before `now` is refused, timeout, and expires.
    SecretEffects expire(std::uint64_t now);
    // The oldest held value whose retry time is at or before `now` is written again: stored when durable (the asker,
    // if not yet answered, hears "stored"), else held, its next retry twice as far off (at most kSecretRetryMaxMs).
    SecretEffects retry(std::uint64_t now, SecretKeeper& keeper);
    // The server is ending: every open request is dropped, unanswered (its asker ends with the server), and every held
    // value is lost, logged "lost" (the one way a value that arrived is not stored). Expired requests are forgotten.
    SecretEffects drop();
    // When expire() next has something to do.
    std::optional<std::uint64_t> deadline() const;
    // When retry() next has something to do.
    std::optional<std::uint64_t> retryAt() const;
    // The state of request `id`: open, holding, or how it ended; none for an id this desk never saw (or long ended: it
    // keeps the most recent kKeptEnded).
    std::optional<SecretState> stateOf(std::uint64_t id) const;
    bool isOpen(std::uint64_t id) const;

    static constexpr std::size_t kKeptEnded = 256;

    // A test seam for the trace walk's faulty run alone (tests/sync/secrets_walk --faulty-echo): from now on a stored
    // answer's log line carries the value, a leak in the code's own path, so the walk shows that its binding sees one.
    // nexus-server never calls it.
    void injectEchoFault() { echoFault_ = true; }

private:
    struct Held {
        SecretRequest request;
        SecretAsker asker;
        SecretValue value;
        bool answered = false;          // its asker heard "timeout" already (it expired before the answer arrived)
        std::uint64_t at = 0;           // when it is written again
        std::uint64_t backoff = 0;      // how long after the next failure
    };
    struct Ended {
        SecretState state = SecretState::Refused;
        SecretRequest request;
        SecretAsker asker;
    };
    void end(std::size_t index, SecretState state, SecretEffects& out);
    void remember(const SecretRequest& r, SecretState state, const SecretAsker& asker);
    bool newest(std::uint64_t id, const std::string& account) const;
    void write(const SecretRequest& r, const SecretAsker& asker, const SecretValue& value, bool answered, bool first,
               SecretKeeper& keeper, std::uint64_t now, std::uint64_t backoff, SecretEffects& out);
    std::vector<SecretRequest> open_;               // oldest first
    std::map<std::uint64_t, SecretAsker> askers_;   // each open request's asker (the ask number is not on the wire)
    std::map<std::uint64_t, std::uint64_t> due_;    // each open request's deadline
    std::map<std::uint64_t, Held> held_;            // each held value, by its request's id
    std::map<std::uint64_t, Ended> ended_;          // the most recent requests ended, by id
    bool echoFault_ = false;
};

// The window's side: the dialog. It shows the oldest open request the window received and has not answered, one at a
// time; Enter sends the value once and closes it, Escape cancels; it closes when the request leaves the state.
class SecretPrompt {
public:
    // The window received the server's state: its open requests, oldest first. The dialog stays while its request is
    // still open; else it closes (what was typed is wiped) and the oldest request not answered opens in its place.
    // Returns true when the dialog changed.
    bool received(const std::vector<SecretRequest>& open);
    // The window lost its link or quit: the dialog, what was typed and what was answered are gone.
    void lost();

    // The request the dialog shows, or null.
    const SecretRequest* showing() const { return shown_ ? &*shown_ : nullptr; }
    const std::vector<SecretRequest>& view() const { return view_; }
    const std::set<std::uint64_t>& answered() const { return done_; }

    // The field's text: what the person typed or pasted. Ignored with no dialog.
    void type(std::string_view text);
    const SecretValue& typed() const { return typed_; }

    // Enter: with text typed, the answer to send (the text with any line breaks and spaces at its ends cut, which a
    // paste brings), and the dialog closes, wiping the field. False with nothing typed, or no dialog.
    bool submit(std::uint64_t* id, SecretValue* value);
    // Escape: the cancel to send, and the dialog closes. False with no dialog.
    bool cancel(std::uint64_t* id);

private:
    void openNext();
    std::vector<SecretRequest> view_;
    std::optional<SecretRequest> shown_;
    SecretValue typed_;
    std::set<std::uint64_t> done_;
};

} // namespace nexus::sync
