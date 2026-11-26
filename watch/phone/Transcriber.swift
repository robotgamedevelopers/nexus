// The recognizer the watch's words go through: Apple's on-device SpeechTranscriber (Speech framework), on the phone
// (watchOS has no Speech framework) and on the Mac that stands in for the phone in the simulator's end-to-end test.
// A recording in, its words out, or why there are none, in words for the person: never silently nothing.

import AVFoundation
import Foundation
import Speech

enum Transcriber {
    enum Outcome: Equatable {
        case words(String)
        case failed(String)
    }

    // The words in the recording at `file`, in `locale`'s language (the device's by default).
    static func transcribe(file: URL, locale: Locale = Locale.current) async -> Outcome {
        guard let supported = await SpeechTranscriber.supportedLocale(equivalentTo: locale) else {
            return .failed("this device's recognizer has no \(locale.identifier) speech")
        }
        let transcriber = SpeechTranscriber(locale: supported, preset: .transcription)
        do {
            if let install = try await AssetInventory.assetInstallationRequest(supporting: [transcriber]) {
                try await install.downloadAndInstall()
            }
        } catch {
            return .failed("the recognizer's \(supported.identifier) model could not be installed: \(error.localizedDescription)")
        }
        let audio: AVAudioFile
        do {
            audio = try AVAudioFile(forReading: file)
        } catch {
            return .failed("the recording could not be read: \(error.localizedDescription)")
        }
        let analyzer = SpeechAnalyzer(modules: [transcriber])
        do {
            async let collected: String = {
                var text = ""
                for try await result in transcriber.results {
                    text += String(result.text.characters)
                }
                return text
            }()
            if let last = try await analyzer.analyzeSequence(from: audio) {
                try await analyzer.finalizeAndFinish(through: last)
            } else {
                await analyzer.cancelAndFinishNow()
            }
            let words = try await collected.trimmingCharacters(in: .whitespacesAndNewlines)
            return words.isEmpty ? .failed("there were no words in the recording") : .words(words)
        } catch {
            return .failed("the recognizer stopped: \(error.localizedDescription)")
        }
    }
}
