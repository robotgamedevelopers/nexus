// The phone's voice glue (src/voice_window.h) over a fake Speech: in an AI's page the microphone is dictation into
// that page's prompt, after what was typed, exactly once, never sent. Pure: the times are the test's numbers, the
// speech a fake that answers as told; no OS, no audio, no clock.

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include "model.h"
#include "view.h"
#include "voice_window.h"

namespace {

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

using nexus::voice::SpeechEvent;

struct FakeSpeech : nexus::voice::Speech {
    nexus::voice::SpeechInbox& inbox;
    int stops = 0;
    explicit FakeSpeech(nexus::voice::SpeechInbox& in) : inbox(in) {}
    void askPermission() override { inbox.post({SpeechEvent::Kind::Permission, true}); }
    void startListening() override {}
    void stopListening() override { ++stops; }
    void speak(std::string_view) override {}
    void stopSpeaking() override {}
};

struct Page {
    nexus::Model model{{{"johnny", ""}}, false};
    nexus::Look look;
    nexus::View view;
    std::unique_ptr<nexus::VoiceWindow> voice;
    FakeSpeech* speech = nullptr;
    double t = 1;
    Page() {
        view.model = &model;
        view.look = &look;
        view.idiom = "phone";
        view.phoneMenu = false;
        view.windowApp.surface.voiceMic = true;
        for (std::size_t i = 0; i < model.tab_count(); ++i)
            if (model.tab(i).name == "johnny") model.select_tab(i);
        voice = std::make_unique<nexus::VoiceWindow>(model, view);
        auto fake = std::make_unique<FakeSpeech>(voice->inbox());
        speech = fake.get();
        voice->setSpeech(std::move(fake));
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) voice->frame(t += 0.05);
    }
    void heard(const std::string& text, bool final) { voice->inbox().post({SpeechEvent::Kind::Heard, final, text}); }
    const std::string& draft() { return model.tab(model.active()).draft; }
};

void dictationGoesIntoThePromptOnce(const std::string& typed) {
    Page p;
    p.model.set_draft(p.model.active(), typed);
    p.frames(2);
    p.view.voice->press();
    p.frames(3);
    CHECK(p.view.voice->listening);
    p.heard("What is left", false);
    p.frames(2);
    const std::string base = typed.empty() ? "" : typed + " ";
    CHECK(p.draft() == base + "What is left");
    p.view.voice->press(); // done: the recognizer finishes, then its final words arrive
    p.frames(2);
    p.heard("What is left on the board?", true);
    p.frames(20);
    if (p.draft() != base + "What is left on the board?")
        std::fprintf(stderr, "  the prompt holds \"%s\"\n", p.draft().c_str());
    CHECK(p.draft() == base + "What is left on the board?");
    CHECK(!p.view.voice->listening);
    CHECK(p.model.tab(p.model.active()).transcript.empty()); // never sent from the microphone
}

// The recognizer may end on its own, its final words arriving while the microphone still listens.
void aFinalWhileListeningGoesInOnce() {
    Page p;
    p.frames(2);
    p.view.voice->press();
    p.frames(3);
    p.heard("What is left on the board?", false);
    p.frames(2);
    p.heard("What is left on the board?", true);
    p.frames(20);
    if (p.draft() != "What is left on the board?")
        std::fprintf(stderr, "  the prompt holds \"%s\"\n", p.draft().c_str());
    CHECK(p.draft() == "What is left on the board?");
    if (p.view.voice->listening) p.view.voice->press();
    p.frames(20);
    CHECK(p.draft() == "What is left on the board?");
}

// The speech's words come from another thread and are read each frame: while it listens, frames go on (else a
// recognizer's words wait for the next touch, as they did on the simulator).
void framesGoOnWhileListening() {
    Page p;
    p.look.prompt = p.view.windowApp.surface.prompt;
    p.look.tabs = p.view.windowApp.surface.tabs;
    p.look.transcriptStyle = p.view.windowApp.transcriptStyle;
    p.look.messageCard = p.view.windowApp.messageCard;
    p.look.textSteps = p.view.askedTextSteps;
    p.frames(2);
    CHECK(!nexus::windowWantsFrame(p.view));
    p.view.voice->press();
    p.frames(3);
    CHECK(p.view.voice->listening);
    CHECK(nexus::windowWantsFrame(p.view));
}

} // namespace

int main() {
    framesGoOnWhileListening();
    aFinalWhileListeningGoesInOnce();
    dictationGoesIntoThePromptOnce("");
    dictationGoesIntoThePromptOnce("Hi Johnny,");
    if (failures) {
        std::fprintf(stderr, "%d voice-window check(s) failed\n", failures);
        return 1;
    }
    std::printf("voice_window: all checks passed\n");
    return 0;
}
