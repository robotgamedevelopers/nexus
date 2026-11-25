// One voice turn of a client: the microphone, what is heard, which AI it goes to, and the reply spoken back.
// models/VoiceTurn.tla is its model, bound to this code by tests/voice/voice_walk.cpp (VoiceTurnTrace.tla).
//
// Pure logic, with no Prax, no OS and no clock: the host (the iPhone's window, src/voice/voice_host.h; later a watch)
// calls it with what happened and the time as an argument, and carries out the acts it returns: asking the OS for the
// microphone and speech recognition, starting and stopping the recognizer and the speech synthesizer, opening a
// AI's tab, and sending a prompt through the same path as a typed one. The server sees an ordinary prompt; which
// AI it is for is worked out here, from the server's list of AIs (name_match.h).
//
// The rules, each a property of the model:
// - The microphone is never on while the reply is being spoken: starting to listen stops the speech first, and a
//   reply arriving while listening waits until the listen ends (NeverHearsItself).
// - Only a reply to a prompt sent by voice is spoken, and only while replies are spoken (on the menu) (SpeaksAnswers,
//   TextIsSilent).
// - Without the microphone and speech recognition granted, nothing listens; a refusal says so, with the fix
//   (DeniedNeverListens, DeniedIsTold).
// - What was heard is sent at most once, to the AI named or the one showing; heard text that cannot be sent (the
//   recognizer failed, two AIs answer to the name, no AI is showing) goes to the prompt's draft, never lost
//   (SentOnce, NothingLost).
// - A listen always ends: past kMaxListenSeconds it is stopped, and a recognizer that gives no final result within
//   kFinishSeconds of being stopped is taken at what it heard (ListenEnds).
#pragma once

#include <deque>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "voice/name_match.h"

namespace nexus::voice {

enum class Phase { Idle, Asking, Listening, Finishing, Denied };
// How replies come back: fixed by where the person is, never a setting (replyOn): spoken on the menu, text in a tab.
enum class Reply { Text, Voice };
Reply replyOn(bool menu);
enum class Permission { Unknown, Granted, Refused };

struct Act {
    enum class Kind {
        AskPermission,  // ask the OS for the microphone and speech recognition; answer with permission()
        StartListening, // start the recognizer on the microphone; answer with heard() and failed()
        StopListening,  // stop the microphone; the recognizer still gives its final result through heard()
        Speak,          // speak `text`, `aiName`'s reply; answer with spoken() when it ends
        StopSpeaking,   // stop the speech now, and drop what it was saying
        Open,           // show `aiName`'s tab
        Send,           // send `text` to `aiName`, as a typed prompt is sent
        Draft,          // put `text` in the draft of `aiName`'s tab (the tab showing when empty), to edit and send
    };
    Kind kind;
    std::string aiName{};
    std::string text{};
    bool operator==(const Act&) const = default;
};

struct Queued {
    std::string aiName, text;
    bool operator==(const Queued&) const = default;
};

class VoiceTurn {
public:
    static constexpr double kMaxListenSeconds = 55; // the recognizer's own limit is about a minute
    static constexpr double kFinishSeconds = 4;

    // What the host sees each frame: the AI whose tab shows (empty for none) and the server's AIs.
    void context(std::string current, std::vector<SpokenAI> ais);

    std::vector<Act> press(double now);                                  // the microphone button
    std::vector<Act> permission(bool granted, std::string_view why);     // the OS's answer to AskPermission
    std::vector<Act> heard(std::string_view text, bool final);           // the recognizer, as it forms and at its end
    std::vector<Act> failed(std::string_view why);                       // the recognizer or the audio session failed
    std::vector<Act> tick(double now);                                   // each frame
    std::vector<Act> replied(std::string_view aiName, std::string_view text); // a block of a reply closed
    std::vector<Act> spoken();                                           // the synthesizer finished an utterance
    void turnEnded(std::string_view aiName);                         // the AI waits for the person again
    std::vector<Act> setReply(Reply reply);                              // text or voice: set from where the person is (replyOn)

    Phase phase() const { return phase_; }
    Permission permitted() const { return permission_; }
    Reply reply() const { return reply_; }
    bool speaking() const { return speaking_; }
    const std::deque<Queued>& queued() const { return queue_; }
    const std::set<std::string, std::less<>>& awaiting() const { return awaiting_; }
    const std::string& heardText() const { return heard_; }
    // One plain line for the person: who the last prompt went to, or why nothing was sent or heard.
    const std::string& line() const { return line_; }
    // The line says what went wrong (permission refused, recognition failed) rather than where a prompt went.
    bool trouble() const { return trouble_; }

private:
    std::vector<Act> listen(double now);
    std::vector<Act> finish();
    void speakNext(std::vector<Act>& acts);

    Phase phase_ = Phase::Idle;
    Permission permission_ = Permission::Unknown;
    Reply reply_ = Reply::Voice;
    bool speaking_ = false;
    bool trouble_ = false;
    std::deque<Queued> queue_;
    std::set<std::string, std::less<>> awaiting_;
    std::string heard_, line_, current_;
    std::vector<SpokenAI> ais_;
    double listenStart_ = 0, stoppedAt_ = 0;
};

const char* phaseName(Phase phase);
const char* replyName(Reply reply);

} // namespace nexus::voice
