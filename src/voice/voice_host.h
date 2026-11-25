// A client's voice, put together: the turn (voice_turn.h) driving the OS's speech (speech.h), and the client's own
// three answers to what the turn decides: show an AI, send a prompt, put text in a draft. No Prax and no window: the
// iPhone's window gives it its model's tabs (src/voice_window.h), and a native watch client gives it its own list.
//
// Once a frame the client calls frame() with the time, the AI showing and the server's AIs; it drains what the
// OS said since (permission, transcript, failures, an utterance done) into the turn and carries out each act.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "voice/speech.h"
#include "voice/voice_turn.h"

namespace nexus::voice {

struct VoiceSurface {
    std::function<void(const std::string& aiName)> open;
    std::function<void(const std::string& aiName, const std::string& text)> send;
    std::function<void(const std::string& aiName, const std::string& text)> draft; // empty name: the tab showing
};

class VoiceHost {
public:
    // `speech` may be null (a platform without it): pressing then says so, and nothing listens.
    VoiceHost(Speech* speech, VoiceSurface surface) : speech_(speech), surface_(std::move(surface)) {}

    SpeechInbox& inbox() { return inbox_; }
    void setSpeech(Speech* speech) { speech_ = speech; } // made over inbox(), so after the host
    const VoiceTurn& turn() const { return turn_; }
    bool available() const { return speech_ != nullptr; }

    void frame(double now, std::string current, std::vector<SpokenAI> ais);
    void press(double now);
    void setReply(Reply reply);
    void replied(const std::string& aiName, const std::string& text);
    void turnEnded(const std::string& aiName) { turn_.turnEnded(aiName); }

private:
    void carry(const std::vector<Act>& acts);

    Speech* speech_;
    VoiceSurface surface_;
    SpeechInbox inbox_;
    VoiceTurn turn_;
};

// A reply's text as it is spoken: Markdown's marks (emphasis, code fences and ticks, headings, list bullets, quotes)
// and a link's address dropped, keeping its words; code blocks are said as "(code)".
std::string speakable(std::string_view markdown);

} // namespace nexus::voice
