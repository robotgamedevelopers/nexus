// The watch's microphone and voice: hold to talk records (AVAudioRecorder, AAC), and what nexus says is spoken
// (AVSpeechSynthesizer). In the simulator's end-to-end test (--speak-file, --voice-to) the recording is a file the test
// made and the speech is written to files rather than the speaker; everything between is the same.

import AVFoundation
import Foundation

enum Microphone {
    // Whether the person has allowed the microphone; asks once when they have not been asked.
    static func allowed() async -> Bool {
        switch AVAudioApplication.shared.recordPermission {
        case .granted: return true
        case .denied: return false
        default: return await AVAudioApplication.requestRecordPermission()
        }
    }
}

final class Recorder {
    private var recorder: AVAudioRecorder?
    private var file: URL?
    var testFiles: [URL] = []   // --speak-file: what the test "says", a file per hold, the last one again after
    private var testFile: URL?
    private(set) var recording = false      // started and not yet stopped

    // Starts recording; why not, in words, on failure.
    func start() -> String? {
        if !testFiles.isEmpty {
            testFile = testFiles.count > 1 ? testFiles.removeFirst() : testFiles[0]
            recording = true
            return nil
        }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("talk.m4a")
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playAndRecord, mode: .spokenAudio)
            try session.setActive(true)
            let r = try AVAudioRecorder(url: url, settings: [
                AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 16000, AVNumberOfChannelsKey: 1,
                AVEncoderAudioQualityKey: AVAudioQuality.medium.rawValue,
            ])
            guard r.record() else { return "the microphone would not start recording" }
            recorder = r
            file = url
            recording = true
            return nil
        } catch {
            return "the microphone would not start: \(error.localizedDescription)"
        }
    }

    // Stops and returns the recording and its file type.
    func stop() -> (Data, String)? {
        recording = false
        if let testFile {
            guard let data = try? Data(contentsOf: testFile) else { return nil }
            return (data, testFile.pathExtension.isEmpty ? "m4a" : testFile.pathExtension)
        }
        recorder?.stop()
        recorder = nil
        defer { if let file { try? FileManager.default.removeItem(at: file) } }
        guard let file, let data = try? Data(contentsOf: file) else { return nil }
        return (data, "m4a")
    }
}

// The synthesizer and its state belong to the main actor; its delegate calls arrive on any thread and hop back.
@MainActor
final class Speaker: NSObject, AVSpeechSynthesizerDelegate {
    private let synthesizer = AVSpeechSynthesizer()
    private var done: (() -> Void)?
    var voiceTo: URL?           // --voice-to: write each utterance to a file in this directory, not the speaker
    private var count = 0
    private var writing: (started: Date, seconds: Double)?
    private var writingTo: URL?     // with voiceTo: the file being spoken into   // with voiceTo: when the utterance began, its length

    override init() {
        super.init()
        synthesizer.delegate = self
    }

    var speaking: Bool { synthesizer.isSpeaking || done != nil }

    func say(_ text: String, then: @escaping () -> Void) {
        let u = AVSpeechUtterance(string: text)
        u.rate = AVSpeechUtteranceDefaultSpeechRate
        done = then
        if let voiceTo {
            count += 1
            let url = voiceTo.appendingPathComponent(String(format: "said-%03d.caf", count))
            writingTo = url
            var file: AVAudioFile?
            // Written as fast as the synthesizer goes, but finished when the speech would have: the turn stays
            // Speaking for as long as it would on the speaker.
            writing = (Date(), 0)
            synthesizer.write(u) { [weak self] buffer in
                guard let pcm = buffer as? AVAudioPCMBuffer else { return }
                if pcm.frameLength == 0 {
                    file = nil
                    Task { @MainActor in self?.finishPaced() }
                    return
                }
                let length = Double(pcm.frameLength) / pcm.format.sampleRate
                Task { @MainActor in self?.writing?.seconds += length }
                if file == nil {
                    file = try? AVAudioFile(forWriting: url, settings: pcm.format.settings,
                                            commonFormat: pcm.format.commonFormat, interleaved: pcm.format.isInterleaved)
                }
                try? file?.write(from: pcm)
            }
            return
        }
        synthesizer.speak(u)
    }

    func stop() {
        synthesizer.stopSpeaking(at: .immediate)
        // With voiceTo the file was written ahead of time; one stopped before its end is marked so, for the test.
        if writing != nil, let url = writingTo {
            try? FileManager.default.moveItem(at: url, to: url.deletingPathExtension().appendingPathExtension("stopped.caf"))
        }
        writing = nil
        done = nil
    }

    // With voiceTo, once the written speech's length has passed since it began.
    private func finishPaced() {
        guard let w = writing else { return finish() }
        let left = max(0, w.seconds - Date().timeIntervalSince(w.started))
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: UInt64(left * 1e9))
            if self.writing?.started == w.started {
                self.writing = nil
                self.finish()
            }
        }
    }

    private func finish() {
        let d = done
        done = nil
        d?()
    }

    nonisolated func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish u: AVSpeechUtterance) {
        Task { @MainActor in self.finishPaced() }
    }
}
