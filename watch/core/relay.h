// The watch reaches nexus-server through its paired iPhone (docs/watch.md says why): the watch sends frames to the
// phone and reads the phone's answer to each, and the phone holds the link to the server (link/client.h) as any window
// does. A frame goes over WatchConnectivity (sendMessageData and its reply) on a real watch, or over HTTP on loopback
// in the simulator's end-to-end test; the bytes are the same.
//
// What it guarantees is the link's own, carried one hop further: nothing the person said is lost or applied twice.
//   - The watch numbers its prompts (1, 2, 3 ...) and keeps each in its outbox, across launches, until the phone says
//     the server has acknowledged it (PhoneFrame::applied), so a frame lost or answered twice changes nothing.
//   - The phone hands each prompt number to the link at most once, ever: before it hands one over it writes down that
//     it did (PhoneRelay::persisted, `handed`), so a phone that restarts never hands it again. A prompt the watch still
//     holds at or below a restarted phone's `handed` may or may not have reached the server, and the phone says so
//     (PhoneFrame::uncertain) rather than guess: the watch tells the person, aloud and on screen. Both are bounds, not
//     lists, so an answer vouches for no prompt it was not asked about: an answer the watch takes may be to an older
//     frame than the outbox it is applied to.
//   - Within one run of the phone, the link applies each action once (models/Link.tla, Reconnect.tla).
// The same frames switch an AI's account (protocol 2): the phone sends each AI's accounts, the watch its newest choice,
// held until the phone says it is done with it (PhoneFrame::switched), and the phone hands it to the AI's first session
// as a SetAccount (watch/models/AccountSwitch.tla).
// watch/models/Relay.tla and AccountSwitch.tla are the models; watch/tests/relay_walk.cpp binds this code to them.
//
// Logic only: no clock, no I/O. Frames are encoded and decoded here; the carriers and the link are the hosts'.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "voice_turn.h"

namespace nexus::watch {

// Protocol 2 appends the account switch to the watch's frame and the accounts to the phone's. A frame of 1 still reads,
// without them; a build of 1 refuses a frame of 2, saying to update both ends.
inline constexpr std::uint64_t kRelayProto = 2;
inline constexpr std::uint64_t kRelayProtoOldest = 1;

// A recording on its way to the phone's recognizer.
struct Audio {
    std::uint64_t utterance = 0;
    std::string format;     // the file type: "m4a", "caf", "wav", "aiff"
    std::string bytes;
    bool operator==(const Audio&) const = default;
};

// An AI's accounts, as the server lists them (sync::ServerAI::accounts, sync::AccountChoice), and the one its
// session runs on now. The watch shows the signed-in ones and switches between them (watch/models/AccountSwitch.tla).
struct AccountOption {
    std::string name;               // the account's id: what a switch names
    std::string label;
    std::string email;
    bool signedIn = false;          // !AccountChoice::needsSignIn: only these can be switched to
    bool operator==(const AccountOption&) const = default;
};

struct AiAccounts {
    std::string aiName;
    std::string current;            // the account the AI's session runs on now; "" for the harness's default
    std::vector<AccountOption> accounts;    // in the server's order
    bool operator==(const AiAccounts&) const = default;
};

// The person's choice of account for an AI, numbered by the watch (1, 2, 3 ... across launches): the watch holds its
// newest until the phone says it is done with it (PhoneFrame::switched).
struct AccountSwitch {
    std::uint64_t num = 0;
    std::string aiName;
    std::string account;
    bool operator==(const AccountSwitch&) const = default;
};

// A row of the watch's account list: what it shows (the email, else the label, else the name).
struct AccountRow {
    std::string account;
    std::string shown;
    bool current = false;           // the account the AI runs on now, as the server last said
    bool switching = false;         // the watch's choice, not yet done
    bool operator==(const AccountRow&) const = default;
};

// The list the watch shows for an AI: its signed-in accounts, in the server's order. A switch the watch holds marks
// its row switching.
std::vector<AccountRow> accountRows(const AiAccounts& ai, const std::optional<AccountSwitch>& held);

struct WatchFrame {
    std::uint64_t proto = kRelayProto;
    std::string watch;              // the watch's id: one phone relays for one watch at a time
    std::uint64_t epoch = 0;        // the phone run the watch's `ack` counts in (PhoneFrame::epoch)
    std::uint64_t ack = 0;          // the phone's events the watch has, up to this number
    std::vector<Prompt> prompts;    // the outbox, oldest first
    std::optional<Audio> audio;     // the recording not yet taken
    std::optional<AccountSwitch> accountSwitch;     // the choice not yet done (protocol 2)
    bool operator==(const WatchFrame&) const = default;
};

enum class LinkPhase : std::uint8_t { Connecting = 0, Connected = 1, Lost = 2 };

struct Event {
    // AccountRefused (protocol 2): a switch the phone could not make, and why; said, and not an answer to a prompt.
    enum class Kind : std::uint8_t { Transcript = 0, TranscriptFailed = 1, Reply = 2, Notice = 3, AccountRefused = 4 };
    std::uint64_t num = 0;
    Kind kind = Kind::Reply;
    std::uint64_t utterance = 0;    // with Transcript and TranscriptFailed
    std::string aiName;         // with Reply, Notice and AccountRefused
    std::string text;               // the words, the reply, or what went wrong
    bool operator==(const Event&) const = default;
};

struct PhoneFrame {
    std::uint64_t proto = kRelayProto;
    std::uint64_t epoch = 0;        // this run of the phone's relay: events are numbered within it
    std::uint64_t uncertain = 0;    // prompts up to this number: the phone cannot vouch for them (a restart while they
                                    // were in flight); 0 for none
    std::uint64_t applied = 0;      // prompts after `uncertain` up to this number: the server has them
    std::uint64_t audioTaken = 0;   // recordings up to this utterance are with the recognizer
    LinkPhase link = LinkPhase::Connecting;
    std::string why;                // with Lost: why, in words for the person
    bool aisKnown = false;
    std::vector<std::string> ais;
    std::vector<Event> events;      // after the watch's ack, oldest first
    // From protocol 2:
    std::uint64_t switched = 0;     // account switches up to this number are done with: the server has applied it (its
                                    // link acknowledged the SetAccount), or the phone refused it and said why (an event)
    std::vector<AiAccounts> rosters;    // each AI's accounts, as the link last showed them; none while locked
    bool operator==(const PhoneFrame&) const = default;
};

std::string encode(const WatchFrame& f);
std::string encode(const PhoneFrame& f);
// False, with why, on bytes that are not a whole frame of this protocol.
bool decode(const std::string& bytes, WatchFrame* out, std::string* why);
bool decode(const std::string& bytes, PhoneFrame* out, std::string* why);

// The watch's end: its outbox and the recording, what it has taken of the phone's events.
class WatchLink {
public:
    explicit WatchLink(std::string watchId = {});

    void add(Prompt p);                         // to the outbox, until applied or lost
    void record(Audio a);                       // the newest recording, until the phone takes it
    WatchFrame frame() const;

    struct Taken {
        std::vector<Event> events;              // new ones, in order
        std::vector<Prompt> lost;               // taken out of the outbox: the phone cannot vouch for them
        std::uint64_t applied = 0;
        LinkPhase link = LinkPhase::Connecting;
        std::string why;
        bool aisKnown = false;
        std::vector<std::string> ais;
    };
    Taken take(const PhoneFrame& f);

    // The person chose `account` for `ai` from the account list. Held, and sent in every frame, until the phone is done
    // with it. True when there is a switch to send. False with `why` empty when there is nothing to do: the account is
    // already the AI's (or the one a switch already held for it names). False with `why` when it cannot be chosen: no
    // list for the AI yet, an account not signed in, or a switch for another AI still under way (one at a time, so
    // that none is dropped unsaid).
    bool choose(const std::string& ai, const std::string& account, std::string* why);
    // The AI's account list as the phone last showed it (accountRows): empty when the phone has sent none for it.
    std::vector<AccountRow> accounts(const std::string& ai) const;
    const std::vector<AiAccounts>& rosters() const { return rosters_; }
    const std::optional<AccountSwitch>& accountSwitch() const { return switch_; }
    std::uint64_t nextSwitch() const { return nextSwitch_; }

    const std::vector<Prompt>& outbox() const { return outbox_; }
    const std::optional<Audio>& audio() const { return audio_; }
    std::uint64_t ack() const { return ack_; }
    std::uint64_t epoch() const { return epoch_; }
    const std::string& id() const { return id_; }
    std::uint64_t nextPrompt() const { return nextPrompt_; }
    std::uint64_t utterances() const { return utterances_; }   // the newest recording's number ever recorded

    // What outlives the app: the id, the outbox, the next prompt number and the recordings' number (the phone keeps
    // the newest it took per watch, so a number reused after a restart would be dropped as already heard).
    std::string save() const;
    static bool load(const std::string& bytes, WatchLink* out, std::string* why);

private:
    std::string id_;
    std::vector<Prompt> outbox_;
    std::uint64_t nextPrompt_ = 1;
    std::uint64_t utterances_ = 0;
    std::optional<Audio> audio_;
    std::uint64_t epoch_ = 0;
    std::uint64_t ack_ = 0;
    bool synced_ = false;                       // ack_ is where this epoch's events stand for this watch
    std::vector<AiAccounts> rosters_;           // as of the last answer taken
    std::optional<AccountSwitch> switch_;       // the choice not yet done
    std::uint64_t nextSwitch_ = 1;              // outlives the app: the phone drops a number it has taken as done
};

// What the phone's link shows the relay: the server's state and the link's, as link::ClientLink's snapshot holds them.
struct LinkEntry {
    enum class Kind : std::uint8_t { Person, Ai, Notice, Tool, Bus };
    Kind kind = Kind::Notice;
    std::string text;
    bool closed = true;
};

struct LinkSession {
    std::uint64_t id = 0;
    std::string aiName;
    enum class Activity : std::uint8_t { Idle, Busy, Down, Working } activity = Activity::Idle;
    std::uint64_t first = 0;            // the transcript index of entries[0]
    std::vector<LinkEntry> entries;     // the transcript's tail
    std::uint64_t total() const { return first + entries.size(); }
};

struct LinkView {
    LinkPhase phase = LinkPhase::Connecting;
    std::string why;
    bool aisKnown = false;
    std::vector<std::string> ais;
    std::vector<LinkSession> sessions;
    std::uint64_t sent = 0;             // actions handed to the link
    bool acknowledged = true;           // the server has acknowledged every one
    std::vector<AiAccounts> rosters;    // each AI's accounts and the one its first session runs on
};

// An action the relay hands the link (sync::Action Ensure, Send and SetAccount).
struct LinkAction {
    enum class Kind : std::uint8_t { Ensure, Send, SetAccount };
    Kind kind = Kind::Send;
    std::uint64_t session = 0;
    std::string aiName;
    std::string text;                   // with Send: the words; with SetAccount: the account
    std::uint64_t prompt = 0;           // with Send: the watch's prompt number; with SetAccount: the switch's
    bool operator==(const LinkAction&) const = default;
};

// The phone's end: one watch's prompts to the link, the server's replies and the recognizer's words back.
class PhoneRelay {
public:
    struct Persisted {
        std::string watch;
        std::uint64_t handed = 0;       // the highest prompt number ever handed to a link
        bool operator==(const Persisted&) const = default;
    };
    // `epoch`: this run's number, different from every earlier run's (the host draws it at random).
    PhoneRelay(Persisted persisted, std::uint64_t epoch);

    struct Exchanged {
        PhoneFrame reply;
        std::optional<Audio> transcribe;    // a new recording for the recognizer
    };
    // One frame from the watch, and the answer to it.
    Exchanged exchange(const WatchFrame& in);
    // The recognizer's words for a recording (ok), or why it found none.
    void transcribed(std::uint64_t utterance, bool ok, const std::string& text);
    // The link's latest state: the actions to hand it now, in order. Before it returns a Send it has advanced
    // persisted().handed: the host writes persisted() durably, then hands the actions over, in that order.
    std::vector<LinkAction> link(const LinkView& view);

    // The owner's check on the phone: its nexus app's lock passed since the phone last slept (true), or not. Locked,
    // the relay takes no prompts or recordings, hands nothing to the link and sends no words back: it answers that
    // nexus on the phone is locked, which the watch says and shows. A new relay starts locked.
    void setUnlocked(bool unlocked) { unlocked_ = unlocked; }
    bool unlocked() const { return unlocked_; }
    static constexpr const char* kLockedWhy = "nexus on your iPhone is locked. Open it on the phone and unlock it.";

    // The answer to a frame as things stand: exchange() makes it, and a host answers again with it after moving the
    // link's latest state in.
    PhoneFrame answer(const WatchFrame& in) const;

    const Persisted& persisted() const { return persisted_; }
    std::uint64_t epoch() const { return epoch_; }
    std::uint64_t acked() const { return acked_; }
    std::uint64_t bootHanded() const { return bootHanded_; }
    const std::deque<Prompt>& waiting() const { return waiting_; }
    const std::deque<Event>& events() const { return events_; }
    std::uint64_t linkSent() const { return linkSent_; }
    // The prompts handed to the link and not yet acknowledged, with the link's count when each was handed.
    const std::deque<std::pair<std::uint64_t, std::uint64_t>>& inflight() const { return inflight_; }
    // The account switches, this run (watch/models/AccountSwitch.tla): the newest number taken, the one waiting to be
    // handed, the one handed and not yet acknowledged (with the link's count when handed), and those done with.
    std::uint64_t switchSeen() const { return switchSeen_; }
    const std::optional<AccountSwitch>& switchWaiting() const { return switchWaiting_; }
    const std::optional<std::pair<AccountSwitch, std::uint64_t>>& switchInflight() const { return switchInflight_; }
    std::uint64_t switched() const { return switched_; }
    // The account the AI's roster names as current in the link's latest state, "" for none.
    std::string currentAccount(const std::string& ai) const;

private:
    void event(Event::Kind kind, std::uint64_t utterance, std::string aiName, std::string text);
    void readReplies(const LinkView& view);

    Persisted persisted_;
    std::uint64_t epoch_ = 0;
    std::uint64_t bootHanded_ = 0;
    std::uint64_t acked_ = 0;
    std::uint64_t audioTaken_ = 0;
    std::deque<Prompt> waiting_;            // received, not yet handed, in number order
    std::deque<std::pair<std::uint64_t, std::uint64_t>> inflight_;
    std::uint64_t linkSent_ = 0;
    std::map<std::string, bool> ensured_;   // an AI with no session: its ensure is handed once
    std::deque<Event> events_;
    std::uint64_t eventNext_ = 1;
    std::uint64_t watchAck_ = 0;
    struct Await {
        std::string aiName;
        std::uint64_t since = 0;            // the transcript length when the prompt was handed
    };
    std::map<std::uint64_t, Await> awaits_; // by session: a reply owed to the watch
    LinkView last_;
    bool unlocked_ = false;
    std::uint64_t switchSeen_ = 0;
    std::optional<AccountSwitch> switchWaiting_;
    std::optional<std::pair<AccountSwitch, std::uint64_t>> switchInflight_;
    std::uint64_t switched_ = 0;
};

} // namespace nexus::watch
