// The window's voice (docs/voice.md): the voice host (src/voice/voice_host.h) on the window's model and view. It gives the
// host the AI showing and the AIs' tabs (with the spoken forms the client policy's voice_names adds), carries
// out what the turn decides through the window's own paths (a prompt sent as a typed one is, through View::submit; a
// tab shown as a tap on the phone's menu shows it), feeds back each AI's reply as its blocks close, and the end of
// its turn from the state cursor, and keeps the voice panel (View::voice) up to date.
//
// Once a frame, before the view is built: frame(now). A client building no voice makes none, and the panel says so.
#pragma once

#include <cstddef>
#include <map>
#include <memory>

#include "model.h"
#include "view.h"
#include "voice/voice_host.h"

namespace nexus {

class VoiceWindow {
public:
    VoiceWindow(Model& model, View& view);
    ~VoiceWindow();
    VoiceWindow(const VoiceWindow&) = delete;
    VoiceWindow& operator=(const VoiceWindow&) = delete;

    voice::SpeechInbox& inbox() { return host_.inbox(); }
    void setSpeech(std::unique_ptr<voice::Speech> speech);
    const voice::VoiceTurn& turn() const { return host_.turn(); }

    void frame(double now);

private:
    std::size_t aiIndex(const std::string& name) const; // the AI's tab, or tab_count() for none
    void send(const std::string& name, const std::string& text);
    void draft(const std::string& name, const std::string& text);

    Model& model_;
    View& view_;
    VoicePanel panel_;
    voice::VoiceHost host_;
    std::unique_ptr<voice::Speech> speech_;
    std::map<std::size_t, std::size_t> seen_;  // by Tab::id: the transcript entries already seen
    std::map<std::size_t, bool> busy_;         // by Tab::id: its turn was open at the last frame
    bool replyChosen_ = false;                 // the person chose text, voice or both: the policy's first choice no longer applies
    double now_ = 0;
};

} // namespace nexus
