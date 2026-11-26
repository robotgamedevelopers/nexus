// The watch's voice turn: the person holds to talk, what they said comes back as words, the AI it is for is chosen
// from them, the prompt goes to the server through the phone, and the AI's reply is spoken aloud (docs/watch.md).
// The decision alone, apart from the microphone, the speech synthesizer and the link, so that it runs in unit tests and
// in the walk TLC checks against watch/models/VoiceTurn.tla. Time is an argument (milliseconds of one clock that
// never goes back); nothing here reads a clock, a file or a socket.
//
// Nothing fails silently: each thing that stops a turn (a microphone the person has not allowed, no server, a lost
// link, a hold too short to be words, words that never came back, no AI to send to, a name that fits two AIs,
// a prompt the phone cannot vouch for) adds a Notice to says(), which the app both speaks and shows.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "ai_match.h"

namespace nexus::watch {

using Ms = std::uint64_t;

enum class Phase : std::uint8_t {
    Idle,           // nothing under way
    Listening,      // the person holds to talk: the microphone records
    Transcribing,   // the recording went to the phone, whose recognizer turns it into words
    Choosing,       // the words are back: which AI they are for is worked out (choose())
    Sending,        // the prompt is in the watch's outbox, until the phone says the server has it
    Waiting,        // the server has it: the AI's reply has not come
    Speaking,       // a reply is being read aloud
    Interrupted,    // the person stopped a reply: what is left waits for resume() or dismiss()
    Disconnected,   // no server through the phone: a press says so, and what is in the outbox waits
};

const char* phaseName(Phase phase);

// The fault planted in a build for the model checks' walks (NEXUS_WATCH_FAULTS: watch/tests), by name; empty for none.
// The trace check must reject every one (watch/tests/CMakeLists.txt). Not in any other build.
#ifdef NEXUS_WATCH_FAULTS
extern std::string gWatchFault;
#endif

// Something the app says aloud and shows. A Reply is an AI's words, read while the phase is Speaking; a Cue
// ("To Emma") and a Notice (what went wrong, and what to do) are short, and said whatever the phase but Listening.
struct Say {
    enum class Kind : std::uint8_t { Cue, Notice, Reply };
    Kind kind = Kind::Cue;
    std::string text;
    std::string aiName;     // with Reply: whose
    bool operator==(const Say&) const = default;
};

// The prompt choose() makes: the outbox's next number, the AI and the words.
struct Prompt {
    std::uint64_t num = 0;
    std::string aiName;
    std::string text;
    bool operator==(const Prompt&) const = default;
};

class VoiceTurn {
public:
    static constexpr Ms kMinTalkMs = 300;       // a shorter hold is a slip of the finger, not words
    static constexpr Ms kTranscribeMs = 15000;  // words not back by then never will be

    // `nextPrompt`: the number the next prompt gets (the outbox's, which outlives the app). `connected`: whether the
    // phone reports a server. `utterances`: the recordings numbered before (saved with the outbox): the next is one
    // past, so a restarted app never reuses a number the phone has already taken.
    explicit VoiceTurn(std::uint64_t nextPrompt = 1, bool connected = false, std::uint64_t utterances = 0);

    // The events. Each returns true when taken; one not taken changes nothing, unless it says why in a Notice.
    bool press(Ms now, bool micAllowed);    // the talk button goes down (or a tap while a reply is read: stop it)
    bool release(Ms now);                   // the talk button comes up: the recording is sent when long enough
    bool transcript(std::uint64_t utterance, const std::string& text);
    bool transcriptFailed(std::uint64_t utterance, const std::string& why);
    // Choosing: the AI from the words (ai_match.h), against the server's AIs. A prompt for the AI named, or
    // with no name the first AI in the server's list, is made and returned; none when the turn ends here, said why.
    std::optional<Prompt> choose();
    bool applied(std::uint64_t upTo);       // the phone: the server has every prompt numbered up to `upTo`
    bool lostPrompt(std::uint64_t num);     // the phone cannot say whether the server got prompt `num`
    void reply(const std::string& aiName, const std::string& text, bool notice);
    bool speechDone();                      // the reply being read has been read
    bool resume();                          // Interrupted: read what is left
    bool dismiss();                         // Interrupted: drop what is left
    void lost(const std::string& why);      // the phone reports no server, or the phone itself is unreachable
    bool back();                            // a server again
    // The owner's check (docs/watch.md): unlocked is the watch unlocked and on the wrist since its passcode. Locked,
    // the turn neither listens nor reads a reply (it says so); replies wait, unread, until unlock().
    void lock();
    void unlock();
    bool tick(Ms now);                      // timeouts
    void setAis(std::vector<std::string> ais);

    Phase phase() const { return phase_; }
    const std::string& current() const { return current_; }    // the AI the last prompt went to
    std::uint64_t utterance() const { return utterance_; }      // the recording's number, from 1
    std::uint64_t heard() const { return heard_; }              // Transcribing: the recording whose words are awaited
    const std::string& words() const { return words_; }         // Choosing and after: what was heard
    // The prompts made and not yet applied, oldest first; sending() the newest, 0 for none.
    const std::vector<std::uint64_t>& unapplied() const { return unapplied_; }
    std::uint64_t sending() const { return unapplied_.empty() ? 0 : unapplied_.back(); }
    std::uint64_t nextPrompt() const { return nextPrompt_; }
    std::uint32_t awaiting() const { return awaiting_; }        // prompts applied whose reply has not come
    bool connected() const { return connected_; }
    bool unlocked() const { return unlocked_; }
    const std::string& why() const { return why_; }             // Disconnected: why, for the screen
    const std::vector<std::string>& ais() const { return ais_; }
    const std::deque<Say>& replies() const { return replies_; } // replies to read, the one being read first
    // What to say and show, oldest first; the app takes them as it says them.
    std::deque<Say>& says() { return says_; }
    const std::deque<Say>& says() const { return says_; }
    std::uint64_t notices() const { return notices_; }          // Notices made so far
    std::uint64_t cues() const { return cues_; }                // Cues made so far

    // The match choose() found last (for the trace and the screen).
    const AiMatch& lastMatch() const { return match_; }

private:
    void notice(std::string text);
    void cue(std::string text);
    // After every event: Idle or Waiting with a reply to read starts reading it; otherwise the phase rests where the
    // prompts put it.
    void settle();
    Phase rest() const;

    Phase phase_ = Phase::Idle;
    bool connected_ = false;
    bool unlocked_ = false;
    static constexpr const char* kNoWordYet = "Reaching your phone.";
    std::string why_ = kNoWordYet;
    std::string current_;
    std::vector<std::string> ais_;
    std::uint64_t utterance_ = 0;
    std::uint64_t heard_ = 0;
    Ms deadline_ = 0;
    std::string words_;
    std::vector<std::uint64_t> unapplied_;
    std::uint64_t nextPrompt_ = 1;
    std::uint32_t awaiting_ = 0;
    Ms listenedAt_ = 0;
    std::deque<Say> replies_;
    std::deque<Say> says_;
    std::uint64_t notices_ = 0;
    std::uint64_t cues_ = 0;
    AiMatch match_;
};

} // namespace nexus::watch
