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
// watch/models/Relay.tla is the model; watch/tests/relay_walk.cpp binds this code to it.
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

inline constexpr std::uint64_t kRelayProto = 1;

// A recording on its way to the phone's recognizer.
struct Audio {
    std::uint64_t utterance = 0;
    std::string format;     // the file type: "m4a", "caf", "wav", "aiff"
    std::string bytes;
    bool operator==(const Audio&) const = default;
};

struct WatchFrame {
    std::uint64_t proto = kRelayProto;
    std::string watch;              // the watch's id: one phone relays for one watch at a time
    std::uint64_t epoch = 0;        // the phone run the watch's `ack` counts in (PhoneFrame::epoch)
    std::uint64_t ack = 0;          // the phone's events the watch has, up to this number
    std::vector<Prompt> prompts;    // the outbox, oldest first
    std::optional<Audio> audio;     // the recording not yet taken
    bool operator==(const WatchFrame&) const = default;
};

enum class LinkPhase : std::uint8_t { Connecting = 0, Connected = 1, Lost = 2 };

struct Event {
    enum class Kind : std::uint8_t { Transcript = 0, TranscriptFailed = 1, Reply = 2, Notice = 3 };
    std::uint64_t num = 0;
    Kind kind = Kind::Reply;
    std::uint64_t utterance = 0;    // with Transcript and TranscriptFailed
    std::string aiName;         // with Reply and Notice
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
};

// An action the relay hands the link (sync::Action Ensure and Send).
struct LinkAction {
    enum class Kind : std::uint8_t { Ensure, Send };
    Kind kind = Kind::Send;
    std::uint64_t session = 0;
    std::string aiName;
    std::string text;
    std::uint64_t prompt = 0;           // with Send: the watch's prompt number
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
};

} // namespace nexus::watch
