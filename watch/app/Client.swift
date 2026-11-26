// The watch app's state: the voice turn and the watch's end of the relay (nexus_watch.h), a carrier to the phone, the
// microphone and the voice. Everything decided is decided in the C++ core; this holds the devices and shows its state.

import Foundation
import SwiftUI

// The simulator's test options (docs/watch.md, the simulator's end to end). They exist only in a Debug build for the
// simulator (NEXUS_WATCH_TEST_OPTIONS: cmake/watchos.cmake): every other build reads no arguments, has no driven wrist
// and no lock bypass, and always talks to the phone.
struct Options {
    var relay: URL?         // --relay <url>: the Mac's relay over HTTP (the simulator's test), not the phone
    var speakFiles: [URL] = []  // --speak-file <file>[,<file>...]: the recordings holds send, in place of the microphone
    var voiceTo: URL?       // --voice-to <dir>: what is said goes to files there, not the speaker
    var log: URL?           // --log <file>: each phase, say and reply, a line each, for the test
    var drive: [String] = []    // --drive "<step>;<step>...": wait:<s>, press, release, lock, unlock (the wrist)

    static func fromArguments() -> Options {
        var o = Options()
#if NEXUS_WATCH_TEST_OPTIONS && targetEnvironment(simulator)
        var args = Array(ProcessInfo.processInfo.arguments.dropFirst())
        while !args.isEmpty {
            let a = args.removeFirst()
            let v = args.first
            switch a {
            case "--relay": o.relay = v.flatMap(URL.init(string:)); args.removeFirst()
            case "--speak-file":
                o.speakFiles = (v ?? "").split(separator: ",").map { URL(fileURLWithPath: String($0)) }
                args.removeFirst()
            case "--voice-to": o.voiceTo = v.map { URL(fileURLWithPath: $0, isDirectory: true) }; args.removeFirst()
            case "--log": o.log = v.map { URL(fileURLWithPath: $0) }; args.removeFirst()
            case "--drive": o.drive = (v ?? "").split(separator: ";").map(String.init); args.removeFirst()
            default: break
            }
        }
#endif
        return o
    }
}

private func take(_ p: UnsafeMutablePointer<CChar>?) -> String {
    defer { nw_free(p) }
    return p.map { String(cString: $0) } ?? ""
}

@MainActor
final class Client: ObservableObject {
    @Published private(set) var phase = Int32(NW_DISCONNECTED)
    @Published private(set) var current = ""
    @Published private(set) var why = ""
    @Published private(set) var replyFrom = ""
    @Published private(set) var status = ""      // the screen's status line (nw_screen)
    @Published private(set) var problem = ""     // the last notice, or why there is no server: shown as well as said
    private var reply = ""
    private var notice = ""

    private let c: OpaquePointer
    private let carrier: Carrier
    private let recorder = Recorder()
    private let speaker = Speaker()
    private let options: Options
    private var inFlight = false
    private var lastExchange = 0.0
    private var sayQueue: [(Int32, String)] = []
    private var readingReply = false
    private let started = ProcessInfo.processInfo.systemUptime
    private var lockWhy = ""
    private var held = false                    // the talk button is down
    private var pressed = 0                     // presses so far: a press's checks are for this one only
    private var pendingPress: Task<Void, Never>?

    init(options: Options) {
        self.options = options
        let defaults = UserDefaults.standard
        var id = defaults.string(forKey: "watch-id") ?? ""
        if id.isEmpty {
            id = "watch-" + UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "").prefix(16)
            defaults.set(id, forKey: "watch-id")
        }
        let saved = defaults.data(forKey: "outbox") ?? Data()
        c = saved.withUnsafeBytes { raw in
            nw_client_new(id, raw.bindMemory(to: CChar.self).baseAddress, saved.count)
        }
        carrier = options.relay.map { LoopbackCarrier(url: $0) as Carrier } ?? PhoneCarrier()
        recorder.testFiles = options.speakFiles
        if let dir = options.voiceTo {
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            speaker.voiceTo = dir
        }
        log("start \(options.relay == nil ? "phone" : "relay \(options.relay!.absoluteString)")")
        Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.beat() }
        }
#if NEXUS_WATCH_TEST_OPTIONS && targetEnvironment(simulator)
        if !options.drive.isEmpty {
            Task { await self.runDrive(options.drive) }
        }
#endif
        refresh()
    }

    private var now: UInt64 { UInt64((ProcessInfo.processInfo.systemUptime - started) * 1000) }

    // The owner's check (Lock.swift): run when the app comes forward and at each press; leaving the screen locks.
    func scene(active: Bool) {
        if active {
            Task { await self.checkOwner() }
        } else {
            nw_lock(c)
            log("locked: the app went to the background")
            refresh()
        }
    }

    @discardableResult
    private func checkOwner() async -> Bool {
#if NEXUS_WATCH_TEST_OPTIONS && targetEnvironment(simulator)
        // A driven test plays the wrist itself (lock, unlock): the simulator has no wrist. Never in another build.
        if !options.drive.isEmpty { return nw_unlocked(c) != 0 }
#endif
        switch await OwnerCheck.run() {
        case .passed:
            lockWhy = ""
            nw_unlock(c)
            refresh()
            return true
        case .refused(let why):
            nw_lock(c)
            log("locked: \(why)")
            lockWhy = why
            refresh()
            return false
        }
    }

    // The talk button. A tap while a reply is read stops it at once. Otherwise the owner's check and the microphone's
    // permission come first, and they take time: a release before they finish cancels the press, so nothing records
    // after the button is up (the turn says the hold was too short).
    func press() {
        held = true
        pressed += 1
        let this = pressed
        pendingPress?.cancel()
        if nw_phase(c) == NW_SPEAKING {
            speaker.stop()
            nw_press(c, now, 0)
            refresh()
            return
        }
        pendingPress = Task {
            await self.checkOwner()
            let mic = await Microphone.allowed()
            guard this == self.pressed else { return }     // a newer press has its own checks
            guard !Task.isCancelled, self.held else {
                // Up before the checks were done: the turn hears a hold too short to be words, and says so.
                let t = self.now
                if nw_press(self.c, t, mic ? 1 : 0) != 0 { nw_release(self.c, t, nil, 0, "m4a") }
                self.refresh()
                return
            }
            self.pendingPress = nil
            if nw_press(self.c, self.now, mic ? 1 : 0) != 0, nw_phase(self.c) == NW_LISTENING,
               let problem = self.recorder.start() {
                self.log("microphone: \(problem)")
            }
            self.refresh()
        }
    }

    func release() {
        held = false
        pendingPress?.cancel()
        pendingPress = nil
        guard nw_phase(c) == NW_LISTENING else {
            nw_release(c, now, nil, 0, "m4a")
            refresh()
            return
        }
        let (data, format) = recorder.stop() ?? (Data(), "m4a")
        data.withUnsafeBytes { raw in
            _ = nw_release(c, now, raw.baseAddress, data.count, format)
        }
        refresh()
        exchange()
    }

    func resume() { nw_resume(c); refresh() }
    func dismiss() { speaker.stop(); nw_dismiss(c); refresh() }

    private func phase(of c: OpaquePointer) -> Int32 { nw_phase(c) }

    private func beat() {
        if nw_tick(c, now) != 0 { refresh() } else { show() }   // the launch's settle can end on a tick (screen.h)
        let wait = phase == NW_TRANSCRIBING || phase == NW_SENDING || phase == NW_WAITING ? 0.25 : 0.5
        if !inFlight && ProcessInfo.processInfo.systemUptime - lastExchange >= wait { exchange() }
    }

    private func exchange() {
        guard !inFlight else { return }
        inFlight = true
        lastExchange = ProcessInfo.processInfo.systemUptime
        var bytes: UnsafeMutablePointer<CChar>?
        let n = nw_frame(c, &bytes)
        let frame = Data(bytes: bytes!, count: n)
        nw_free(bytes)
        carrier.exchange(frame) { [weak self] result in
            guard let self else { return }
            self.inFlight = false
            switch result {
            case .success(let data):
                data.withUnsafeBytes { raw in
                    _ = nw_take(self.c, raw.bindMemory(to: CChar.self).baseAddress, data.count)
                }
            case .failure(let e):
                nw_carrier_failed(self.c, e.why)
            }
            self.refresh()
        }
    }

    private func refresh() {
        let p = nw_phase(c)
        // The microphone records only while the turn is Listening: when Listening ends for any reason (the watch
        // locked, the server lost, a refusal) the recording stops and is thrown away.
        if p != NW_LISTENING && recorder.recording {
            _ = recorder.stop()
            log("microphone stopped: the turn is \(String(cString: nw_phase_name(p)))")
        }
        if p != phase { log("phase \(String(cString: nw_phase_name(p)))") }
        phase = p
        current = take(nw_current(c))
        why = take(nw_why(c))
        var kind: Int32 = 0
        var text: UnsafeMutablePointer<CChar>?
        while nw_next_say(c, &kind, &text) != 0 {
            let t = take(text)
            log(kind == NW_NOTICE ? "notice \(t)" : "cue \(t)")
            if kind == NW_NOTICE { notice = t }
            sayQueue.append((kind, t))
        }
        var from: UnsafeMutablePointer<CChar>?
        if nw_reply(c, &kind, &text, &from) != 0 {
            reply = take(text)
            replyFrom = take(from)
            notice = ""
        }
        if p == NW_LISTENING { notice = "" }    // a new turn: the last one's problem is over
        show()
        // A reply stops the moment the turn stops reading it: interrupted, or the watch locked.
        if readingReply && p != NW_SPEAKING {
            speaker.stop()
            readingReply = false
            log("reply stopped")
        }
        if p == NW_LISTENING || p == NW_INTERRUPTED {
            persist()
            return
        }
        speakNext()
        persist()
    }

    // The screen's two lines, from the core (nw_screen); published only when they change, so SwiftUI redraws nothing
    // that stayed the same.
    private func show() {
        var line: UnsafeMutablePointer<CChar>?
        var shown: UnsafeMutablePointer<CChar>?
        nw_screen(c, notice, lockWhy, replyFrom, &line, &shown)
        let newStatus = take(line)
        let newProblem = take(shown)
        if newStatus != status { status = newStatus }
        if newProblem != problem { problem = newProblem }
    }

    // Cues and notices first, then the reply being read; never over the microphone.
    private func speakNext() {
        guard !speaker.speaking, phase != NW_LISTENING else { return }
        if !sayQueue.isEmpty {
            let (_, text) = sayQueue.removeFirst()
            speaker.say(text) { [weak self] in self?.refresh() }
            return
        }
        if phase == NW_SPEAKING && !readingReply {
            readingReply = true
            log("reply \(replyFrom): \(reply)")
            speaker.say(reply) { [weak self] in
                guard let self else { return }
                self.readingReply = false
                if self.phase == NW_SPEAKING { nw_speech_done(self.c) }
                self.log("spoken")
                self.refresh()
            }
        }
    }

    private func persist() {
        var bytes: UnsafeMutablePointer<CChar>?
        let n = nw_saved(c, &bytes)
        UserDefaults.standard.set(Data(bytes: bytes!, count: n), forKey: "outbox")
        nw_free(bytes)
    }

    private func log(_ line: String) {
        guard let url = options.log else { return }
        let stamped = String(format: "%.3f ", ProcessInfo.processInfo.systemUptime - started) + line + "\n"
        if let h = try? FileHandle(forWritingTo: url) {
            h.seekToEndOfFile()
            h.write(stamped.data(using: .utf8)!)
            try? h.close()
        } else {
            try? stamped.data(using: .utf8)!.write(to: url)
        }
    }

#if NEXUS_WATCH_TEST_OPTIONS && targetEnvironment(simulator)
    private func runDrive(_ steps: [String]) async {
        for step in steps {
            if step.hasPrefix("wait:"), let s = Double(step.dropFirst(5)) {
                try? await Task.sleep(nanoseconds: UInt64(s * 1e9))
            } else if step == "press" {
                press()
            } else if step == "release" {
                release()
            } else if step == "lock" {
                nw_lock(c)
                refresh()
            } else if step == "unlock" {
                nw_unlock(c)
                refresh()
            }
            log("drove \(step)")
        }
    }
#endif
}
