// The OS's speech, behind a small seam: asking for the microphone and speech recognition, recognizing what is said on
// the device as it forms, and speaking a reply. The voice turn (voice_turn.h) decides; a Speech only carries out its
// acts, and what the OS answers comes back as events in a SpeechInbox, posted from the OS's threads and drained on the
// host's own thread once a frame, so that no frame ever waits on audio (AGENTS.md, Display).
//
// speech_apple.mm is the one implementation: Apple's Speech framework (SFSpeechRecognizer, on-device recognition only:
// what is said never leaves the device) and AVFoundation (AVAudioEngine for the microphone, AVSpeechSynthesizer for the
// reply, AVAudioSession on iOS). It needs no Prax and no window. watchOS has no Speech framework, so a watch client
// brings a Speech of its own for listening and reuses the rest (docs/voice.md, A watch).
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::voice {

struct SpeechEvent {
    enum class Kind {
        Permission, // `ok`: the microphone and speech recognition are both granted; else `text` says which is not and the fix
        Heard,      // `text`: the transcript so far; `ok`: it is final
        Failed,     // `text`: why recognition or the audio stopped, and what to do
        Spoken,     // an utterance finished or was stopped
    };
    Kind kind;
    bool ok = false;
    std::string text{};
};

// Events from any thread to the host's: post() from the OS's callbacks, drain() once a frame.
class SpeechInbox {
public:
    void post(SpeechEvent event) {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
    }
    std::vector<SpeechEvent> drain() {
        std::lock_guard lock(mutex_);
        std::vector<SpeechEvent> out;
        out.swap(events_);
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<SpeechEvent> events_;
};

class Speech {
public:
    virtual ~Speech() = default;
    virtual void askPermission() = 0;
    virtual void startListening() = 0;
    virtual void stopListening() = 0;
    virtual void speak(std::string_view text) = 0;
    virtual void stopSpeaking() = 0;
};

// Apple's speech (speech_apple.mm), posting to `inbox`, which must outlive it. `locale` is a BCP 47 tag, or empty for
// the device's. Returns null on a platform without it.
std::unique_ptr<Speech> makeAppleSpeech(SpeechInbox& inbox, std::string locale = {});

// Recognizes a recorded audio file on the device, as the microphone's audio is recognized: the functional test's way to
// feed known speech through the same recognizer (tests/ios). Posts Heard events, the last final, or Failed, to `inbox`.
void recognizeFile(SpeechInbox& inbox, const std::string& path, std::string locale = {});

} // namespace nexus::voice
