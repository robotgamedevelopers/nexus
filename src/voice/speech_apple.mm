// Apple's speech for the voice turn (speech.h): Speech's on-device recognizer and AVFoundation's microphone and speech
// synthesizer. Built with ARC. Each callback the OS makes, on whatever thread, only posts an event to the inbox.

#include "voice/speech.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <Speech/Speech.h>

#include <atomic>

using nexus::voice::SpeechEvent;
using nexus::voice::SpeechInbox;

@interface NexusSpeechVoice : NSObject <AVSpeechSynthesizerDelegate>
@property(nonatomic, assign) SpeechInbox* inbox;
@property(nonatomic, strong) AVSpeechSynthesizer* synthesizer;
@property(nonatomic, strong) AVSpeechUtterance* current;
@end

@implementation NexusSpeechVoice
- (void)speechSynthesizer:(AVSpeechSynthesizer*)synthesizer didFinishSpeechUtterance:(AVSpeechUtterance*)utterance {
    (void)synthesizer;
    // Only the utterance last asked for: one stopped (stopSpeaking) is the turn's own doing, and one replaced since
    // must not end the next early.
    if (utterance == self.current) self.inbox->post({SpeechEvent::Kind::Spoken, true});
}
@end

namespace nexus::voice {
namespace {

NSString* text(std::string_view s) {
    NSString* out = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return out != nil ? out : @"";
}

std::string utf8(NSString* s) {
    const char* bytes = s != nil ? s.UTF8String : nullptr;
    return bytes != nullptr ? std::string(bytes) : std::string();
}

SFSpeechRecognizer* recognizerFor(const std::string& locale) {
    if (locale.empty()) return [[SFSpeechRecognizer alloc] init];
    return [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale localeWithLocaleIdentifier:text(locale)]];
}

// Why recognition cannot start on this device, or empty when it can. On-device only: what is said never leaves the
// device, so a recognizer that would need Apple's servers is refused, never used.
std::string unavailable(SFSpeechRecognizer* recognizer, const std::string& locale) {
    const std::string which = locale.empty() ? utf8(NSLocale.currentLocale.localeIdentifier) : locale;
    if (recognizer == nil)
        return "Speech recognition does not support the language " + which + ". Set the device's language to one it "
               "supports (Settings > General > Language & Region).";
    if (!recognizer.supportsOnDeviceRecognition)
        return "On-device speech recognition is not available for " + which + " on this device, and nexus never "
               "sends what you say to a server. Turn on Siri once (Settings > Siri) so the device downloads its speech "
               "model, or pick a language with on-device support.";
    if (!recognizer.isAvailable)
        return "Speech recognition is not available right now. Try again in a moment.";
    return {};
}

// A recognizer's error as one plain line. "No speech detected" is not a failure: it is an empty transcript.
bool silence(NSError* error) {
    return [error.domain isEqualToString:@"kAFAssistantErrorDomain"] && (error.code == 1110 || error.code == 1101);
}

std::string failure(NSError* error) {
    return "Speech recognition stopped: " + utf8(error.localizedDescription) + " (" + utf8(error.domain) + " " +
           std::to_string(long(error.code)) + "). Tap the microphone to try again.";
}

class AppleSpeech final : public Speech {
public:
    AppleSpeech(SpeechInbox& inbox, std::string locale) : inbox_(inbox), locale_(std::move(locale)) {
        queue_ = dispatch_queue_create("nexus.speech", DISPATCH_QUEUE_SERIAL);
        voice_ = [[NexusSpeechVoice alloc] init];
        voice_.inbox = &inbox_;
        voice_.synthesizer = [[AVSpeechSynthesizer alloc] init];
        voice_.synthesizer.delegate = voice_;
        engine_ = [[AVAudioEngine alloc] init];
    }

    // Every call runs on the speech's own serial queue, in order: starting the audio session and the engine takes the
    // OS hundreds of milliseconds, and no frame waits on audio (speech.h). The destructor waits for what is queued.
    ~AppleSpeech() override {
        dispatch_sync(queue_, ^{
            stopAudio();
            [task_ cancel];
            [voice_.synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        });
    }

    void askPermission() override { dispatch_async(queue_, ^{ askNow(); }); }
    void startListening() override { dispatch_async(queue_, ^{ listenNow(); }); }
    void stopListening() override { dispatch_async(queue_, ^{ stopNow(); }); }
    void speak(std::string_view say) override {
        std::string copy(say);
        dispatch_async(queue_, ^{ speakNow(copy); });
    }
    void stopSpeaking() override { dispatch_async(queue_, ^{ quietNow(); }); }

private:

    void askNow() {
        SpeechInbox* inbox = &inbox_;
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
            if (status != SFSpeechRecognizerAuthorizationStatusAuthorized) {
                inbox->post({SpeechEvent::Kind::Permission, false,
                    status == SFSpeechRecognizerAuthorizationStatusRestricted
                        ? "Speech recognition is restricted on this device (Screen Time or a profile). Lift the "
                          "restriction in Settings > Screen Time, then tap the microphone again."
                        : "Speech recognition is not allowed for nexus. Allow it in Settings > Privacy & Security > "
                          "Speech Recognition, then tap the microphone again."});
                return;
            }
            auto microphone = ^(BOOL granted) {
                inbox->post({SpeechEvent::Kind::Permission, bool(granted), granted ? std::string() :
                    "The microphone is not allowed for nexus. Allow it in Settings > Privacy & Security > Microphone, "
                    "then tap the microphone again."});
            };
            [AVAudioApplication requestRecordPermissionWithCompletionHandler:microphone]; // iOS 17, watchOS 10, macOS 14
        }];
    }

    void listenNow() {
        [task_ cancel];
        task_ = nil;
        stopAudio();
        const unsigned generation = ++generation_;
        SFSpeechRecognizer* recognizer = recognizerFor(locale_);
        if (auto why = unavailable(recognizer, locale_); !why.empty()) {
            inbox_.post({SpeechEvent::Kind::Failed, false, why});
            return;
        }
        NSError* error = nil;
#if TARGET_OS_IOS || TARGET_OS_WATCH
        AVAudioSession* session = [AVAudioSession sharedInstance];
        if (![session setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault
                          options:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothHFP
                            error:&error] ||
                ![session setActive:YES withOptions:0 error:&error]) {
            inbox_.post({SpeechEvent::Kind::Failed, false, "The microphone could not start: " +
                utf8(error.localizedDescription) + ". Close any app recording audio, then tap the microphone again."});
            return;
        }
#endif
        request_ = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
        request_.requiresOnDeviceRecognition = YES;
        request_.shouldReportPartialResults = YES;
        request_.addsPunctuation = YES;
        AVAudioInputNode* input = engine_.inputNode;
        AVAudioFormat* format = [input outputFormatForBus:0];
        if (format.sampleRate <= 0 || format.channelCount == 0) {
            inbox_.post({SpeechEvent::Kind::Failed, false,
                "No microphone is available. Connect one (or allow it in Settings), then tap the microphone again."});
            return;
        }
        SFSpeechAudioBufferRecognitionRequest* request = request_;
        [input installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
            [request appendAudioPCMBuffer:buffer];
        }];
        tapped_ = true;
        [engine_ prepare];
        if (![engine_ startAndReturnError:&error]) {
            stopAudio();
            inbox_.post({SpeechEvent::Kind::Failed, false, "The microphone could not start: " +
                utf8(error.localizedDescription) + ". Tap the microphone to try again."});
            return;
        }
        SpeechInbox* inbox = &inbox_;
        std::atomic<unsigned>* current = &generation_;
        task_ = [recognizer recognitionTaskWithRequest:request
                                         resultHandler:^(SFSpeechRecognitionResult* result, NSError* failed) {
            if (current->load() != generation) return; // a listen since replaced this one
            if (result != nil) {
                inbox->post({SpeechEvent::Kind::Heard, bool(result.isFinal),
                    utf8(result.bestTranscription.formattedString)});
                if (result.isFinal) return;
            }
            if (failed != nil) {
                if (silence(failed)) inbox->post({SpeechEvent::Kind::Heard, true, std::string()});
                else inbox->post({SpeechEvent::Kind::Failed, false, failure(failed)});
            }
        }];
    }

    void stopNow() {
        stopAudio();
        [request_ endAudio]; // the recognizer gives its final result through the task's handler
    }

    void speakNow(const std::string& say) {
#if TARGET_OS_IOS || TARGET_OS_WATCH
        NSError* error = nil;
        AVAudioSession* session = [AVAudioSession sharedInstance];
        [session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeSpokenAudio options:0 error:&error];
        [session setActive:YES withOptions:0 error:&error];
#endif
        AVSpeechUtterance* utterance = [AVSpeechUtterance speechUtteranceWithString:text(say)];
        if (!locale_.empty()) utterance.voice = [AVSpeechSynthesisVoice voiceWithLanguage:text(locale_)];
        voice_.current = utterance;
        [voice_.synthesizer speakUtterance:utterance];
    }

    void quietNow() {
        voice_.current = nil;
        [voice_.synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    }

    void stopAudio() {
        if (engine_.isRunning) [engine_ stop];
        if (tapped_) [engine_.inputNode removeTapOnBus:0];
        tapped_ = false;
    }

    SpeechInbox& inbox_;
    std::string locale_;
    NexusSpeechVoice* voice_ = nil;
    AVAudioEngine* engine_ = nil;
    SFSpeechAudioBufferRecognitionRequest* request_ = nil;
    SFSpeechRecognitionTask* task_ = nil;
    std::atomic<unsigned> generation_{0};
    bool tapped_ = false;
    dispatch_queue_t queue_ = nil;
};

} // namespace

namespace {
void recognizeFileNow(SpeechInbox& inbox, const std::string& path, const std::string& locale);
}

std::unique_ptr<Speech> makeAppleSpeech(SpeechInbox& inbox, std::string locale) {
    return std::make_unique<AppleSpeech>(inbox, std::move(locale));
}

void recognizeFile(SpeechInbox& inbox, const std::string& path, std::string locale) {
    // Off the caller's thread, as the microphone's listen is: making the recognizer can take the OS a while.
    SpeechInbox* into = &inbox;
    std::string file = path;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ recognizeFileNow(*into, file, locale); });
}

namespace {
void recognizeFileNow(SpeechInbox& inbox, const std::string& path, const std::string& locale) {
    SFSpeechRecognizer* recognizer = recognizerFor(locale);
    if (auto why = unavailable(recognizer, locale); !why.empty()) {
        inbox.post({SpeechEvent::Kind::Failed, false, why});
        return;
    }
    NSURL* url = [NSURL fileURLWithPath:text(path)];
    if (![[NSFileManager defaultManager] isReadableFileAtPath:url.path]) {
        inbox.post({SpeechEvent::Kind::Failed, false, "No readable audio file at " + path + "."});
        return;
    }
    SFSpeechURLRecognitionRequest* request = [[SFSpeechURLRecognitionRequest alloc] initWithURL:url];
    request.requiresOnDeviceRecognition = YES;
    request.shouldReportPartialResults = YES;
    request.addsPunctuation = YES;
    SpeechInbox* target = &inbox;
    [recognizer recognitionTaskWithRequest:request resultHandler:^(SFSpeechRecognitionResult* result, NSError* failed) {
        if (result != nil) {
            target->post({SpeechEvent::Kind::Heard, bool(result.isFinal), utf8(result.bestTranscription.formattedString)});
            if (result.isFinal) return;
        }
        if (failed != nil) {
            if (silence(failed)) target->post({SpeechEvent::Kind::Heard, true, std::string()});
            else target->post({SpeechEvent::Kind::Failed, false, failure(failed)});
        }
    }];
}
} // namespace

} // namespace nexus::voice
