// Unit tests of nexus::voice::VoiceHost (src/voice/voice_host.h): the voice turn driving a speech seam and a client's
// surface. The OS is a fake Speech that records what it is asked; its answers are posted to the inbox as the OS's
// threads post them. Pure logic: no audio, no clock, no threads.

#include "voice/voice_host.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace nexus::voice;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

struct FakeSpeech final : Speech {
    std::vector<std::string> calls;
    void askPermission() override { calls.push_back("ask"); }
    void startListening() override { calls.push_back("listen"); }
    void stopListening() override { calls.push_back("stop"); }
    void speak(std::string_view text) override { calls.push_back("speak:" + std::string(text)); }
    void stopSpeaking() override { calls.push_back("hush"); }
};

struct Client {
    std::vector<std::string> did;
    VoiceSurface surface() {
        return {
            [this](const std::string& f) { did.push_back("open:" + f); },
            [this](const std::string& f, const std::string& t) { did.push_back("send:" + f + ":" + t); },
            [this](const std::string& f, const std::string& t) { did.push_back("draft:" + f + ":" + t); },
        };
    }
};

const std::vector<SpokenAI> kAIs = {{"emma", {}}, {"rowan", {}}};

void speakToTheRightAIAndHearTheReply() {
    FakeSpeech speech;
    Client client;
    VoiceHost host(&speech, client.surface());
    host.press(0);
    CHECK((speech.calls == std::vector<std::string>{"ask"}));
    host.inbox().post({SpeechEvent::Kind::Permission, true});
    host.frame(0.1, "emma", kAIs);
    CHECK((speech.calls == std::vector<std::string>{"ask", "listen"}));
    host.inbox().post({SpeechEvent::Kind::Heard, false, "Rowan,"});
    host.frame(0.5, "emma", kAIs);
    CHECK(host.turn().heardText() == "Rowan,");
    CHECK(client.did.empty());
    host.inbox().post({SpeechEvent::Kind::Heard, true, "Rowan, ship it."});
    host.frame(1, "emma", kAIs);
    CHECK((speech.calls == std::vector<std::string>{"ask", "listen", "stop"}));
    CHECK((client.did == std::vector<std::string>{"open:rowan", "send:rowan:ship it."}));
    CHECK(host.turn().line() == "To Rowan");
    host.replied("rowan", "Shipped 1.4.2.");
    CHECK(speech.calls.back() == "speak:Shipped 1.4.2.");
    host.inbox().post({SpeechEvent::Kind::Spoken, true});
    host.frame(5, "rowan", kAIs);
    CHECK(!host.turn().speaking());
}

void aRefusalFromTheOsIsShown() {
    FakeSpeech speech;
    Client client;
    VoiceHost host(&speech, client.surface());
    host.press(0);
    host.inbox().post({SpeechEvent::Kind::Permission, false, "The microphone is not allowed for nexus. Allow it in "
        "Settings > Privacy & Security > Microphone, then tap the microphone again."});
    host.frame(0.1, "emma", kAIs);
    CHECK(host.turn().phase() == Phase::Denied);
    CHECK(host.turn().line().find("Settings > Privacy & Security > Microphone") != std::string::npos);
    CHECK((speech.calls == std::vector<std::string>{"ask"}));
}

void noSpeechOnThisPlatformSaysSo() {
    Client client;
    VoiceHost host(nullptr, client.surface());
    CHECK(!host.available());
    host.press(0);
    host.frame(0, "emma", kAIs);
    CHECK(host.turn().phase() == Phase::Denied);
    CHECK(host.turn().line().find("no speech recognition") != std::string::npos);
}

void aRecognizerFailureDraftsWhatWasHeard() {
    FakeSpeech speech;
    Client client;
    VoiceHost host(&speech, client.surface());
    host.press(0);
    host.inbox().post({SpeechEvent::Kind::Permission, true});
    host.inbox().post({SpeechEvent::Kind::Heard, false, "the build is"});
    host.inbox().post({SpeechEvent::Kind::Failed, false, "Speech recognition stopped: interrupted."});
    host.frame(1, "emma", kAIs);
    CHECK((client.did == std::vector<std::string>{"draft:emma:the build is"}));
}

void theListenEndsOnItsOwn() {
    FakeSpeech speech;
    Client client;
    VoiceHost host(&speech, client.surface());
    host.press(0);
    host.inbox().post({SpeechEvent::Kind::Permission, true});
    host.frame(0, "emma", kAIs);
    host.inbox().post({SpeechEvent::Kind::Heard, false, "status please"});
    host.frame(VoiceTurn::kMaxListenSeconds, "emma", kAIs);
    CHECK(speech.calls.back() == "stop");
    host.frame(VoiceTurn::kMaxListenSeconds + VoiceTurn::kFinishSeconds, "emma", kAIs);
    CHECK((client.did == std::vector<std::string>{"send:emma:status please"}));
}

void repliesAreSpokenWithoutTheirMarks() {
    CHECK(speakable("**Green.** Two cards left: `#1203` and *#1207*.") == "Green. Two cards left: #1203 and #1207.");
    CHECK(speakable("## Status\n- one\n- two") == "Status\none\ntwo");
    CHECK(speakable("See [the PR](https://github.com/x/y/pull/9) now.") == "See the PR now.");
    CHECK(speakable("Run:\n```sh\nctest -L unit\n```\nThen push.") == "Run:\n(code)\nThen push.");
    CHECK(speakable("> quoted\nsnake_case_name stays") == "quoted\nsnake_case_name stays");
    CHECK(speakable("").empty());
}

} // namespace

int main() {
    repliesAreSpokenWithoutTheirMarks();
    speakToTheRightAIAndHearTheReply();
    aRefusalFromTheOsIsShown();
    noSpeechOnThisPlatformSaysSo();
    aRecognizerFailureDraftsWhatWasHeard();
    theListenEndsOnItsOwn();
    if (failures) {
        std::fprintf(stderr, "%d voice-host check(s) failed\n", failures);
        return 1;
    }
    std::printf("voice_host: all checks passed\n");
    return 0;
}
