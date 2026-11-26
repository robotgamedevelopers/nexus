// nexus-watch-relay: the phone's end of the watch's relay, run on a Mac, where it stands in for the iPhone in the watch
// simulator's end-to-end test (docs/watch.md). The same relay and link the iPhone app runs (nexus_relay.h) and the same
// recognizer (Transcriber.swift); only the carrier differs: HTTP on loopback here (POST /relay, a frame each way),
// WatchConnectivity on the phone. It listens on 127.0.0.1 only.
//
// Usage: nexus-watch-relay --port <n> --state <file> [--host <ssh host>] [--locked] [-- <command run there>...]
//        (--locked: stand for a phone whose nexus app has not been unlocked; the relay refuses the watch, saying so.
//        Otherwise the Mac's own login stands for the phone app's lock.)
//        nexus-watch-relay --transcribe <recording>    (prints the recognizer's words, for the speech test)

import Foundation
import Network

func fail(_ text: String) -> Never {
    FileHandle.standardError.write(("nexus-watch-relay: " + text + "\n").data(using: .utf8)!)
    exit(1)
}

func say(_ text: String) {
    FileHandle.standardError.write(("nexus-watch-relay: " + text + "\n").data(using: .utf8)!)
}

var args = Array(CommandLine.arguments.dropFirst())
var port: UInt16 = 0
var locked = false
var statePath = ""
var host = "localhost"
var command: [String] = []

if args.count == 2 && args[0] == "--transcribe" {
    let done = DispatchSemaphore(value: 0)
    var code: Int32 = 0
    Task {
        switch await Transcriber.transcribe(file: URL(fileURLWithPath: args[1]), locale: Locale(identifier: "en-US")) {
        case .words(let w): print(w)
        case .failed(let why): say("no words: \(why)"); code = 1
        }
        done.signal()
    }
    done.wait()
    exit(code)
}

while !args.isEmpty {
    let a = args.removeFirst()
    switch a {
    case "--port":
        guard let v = args.first.flatMap({ UInt16($0) }), v > 0 else { fail("--port needs a port number") }
        port = v; args.removeFirst()
    case "--state":
        guard let v = args.first else { fail("--state needs a file") }
        statePath = v; args.removeFirst()
    case "--host":
        guard let v = args.first else { fail("--host needs an ssh host") }
        host = v; args.removeFirst()
    case "--locked":
        locked = true
    case "--":
        command = args; args = []
    default:
        fail("unknown argument \(a). Usage: nexus-watch-relay --port <n> --state <file> [--host <h>] [-- <command>...]")
    }
}
if port == 0 || statePath.isEmpty { fail("--port and --state are needed") }

var why: UnsafeMutablePointer<CChar>?
let relay: OpaquePointer = {
    var cargs = command.map { strdup($0) }
    defer { cargs.forEach { free($0) } }
    let made = cargs.withUnsafeMutableBufferPointer { buf -> OpaquePointer? in
        buf.baseAddress!.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: buf.count) { p in
            nr_host_new(host, command.isEmpty ? nil : p, command.count, statePath, &why)
        }
    }
    guard let made else { fail(why.map { String(cString: $0) } ?? "the relay could not start") }
    return made
}()

nr_host_set_unlocked(relay, locked ? 0 : 1)
if locked { say("locked: the watch is refused until nexus on the phone is unlocked") }

let queue = DispatchQueue(label: "relay")

// The recognizer's words for a recording, back to the relay, on the relay's queue (it is not safe across threads).
func transcribe(_ utterance: UInt64, _ bytes: Data, _ format: String) {
    let file = FileManager.default.temporaryDirectory.appendingPathComponent("nexus-watch-\(utterance).\(format)")
    func back(_ ok: Bool, _ text: String) {
        queue.async { nr_host_transcribed(relay, utterance, ok ? 1 : 0, text) }
    }
    Task {
        do {
            try bytes.write(to: file)
        } catch {
            back(false, "the recording could not be kept: \(error.localizedDescription)")
            return
        }
        let outcome = await Transcriber.transcribe(file: file)
        try? FileManager.default.removeItem(at: file)
        switch outcome {
        case .words(let w):
            say("heard recording \(utterance)")
            back(true, w)
        case .failed(let why):
            say("recording \(utterance): \(why)")
            back(false, why)
        }
    }
}

func exchange(_ body: Data) -> (Int, Data) {
    var reply: UnsafeMutablePointer<CChar>?
    var utterance: UInt64 = 0
    var audio: UnsafeMutablePointer<CChar>?
    var audioLen = 0
    var format: UnsafeMutablePointer<CChar>?
    var problem: UnsafeMutablePointer<CChar>?
    let n = body.withUnsafeBytes { raw in
        nr_host_exchange(relay, raw.bindMemory(to: CChar.self).baseAddress, body.count, &reply, &utterance, &audio,
                         &audioLen, &format, &problem)
    }
    defer { nr_free(reply); nr_free(audio); nr_free(format); nr_free(problem) }
    if n == 0 {
        let text = problem.map { String(cString: $0) } ?? "the relay did not answer"
        say(text)
        return (500, text.data(using: .utf8)!)
    }
    if utterance != 0, let audio {
        transcribe(utterance, Data(bytes: audio, count: audioLen), format.map { String(cString: $0) } ?? "m4a")
    }
    return (200, Data(bytes: reply!, count: n))
}

// One HTTP/1.1 request per connection: headers, a body of Content-Length bytes, one answer, closed.
func serve(_ c: NWConnection) {
    var buffer = Data()
    func respond(_ status: Int, _ body: Data) {
        var head = "HTTP/1.1 \(status) \(status == 200 ? "OK" : "Error")\r\nContent-Type: application/octet-stream\r\n"
        head += "Content-Length: \(body.count)\r\nConnection: close\r\n\r\n"
        c.send(content: head.data(using: .utf8)! + body, completion: .contentProcessed { _ in c.cancel() })
    }
    func read() {
        c.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { data, _, complete, error in
            if let data { buffer.append(data) }
            if let end = buffer.range(of: Data("\r\n\r\n".utf8)) {
                let head = String(decoding: buffer[..<end.lowerBound], as: UTF8.self)
                let lines = head.components(separatedBy: "\r\n")
                let length = lines.compactMap { line -> Int? in
                    let parts = line.split(separator: ":", maxSplits: 1)
                    return parts.count == 2 && parts[0].lowercased() == "content-length"
                        ? Int(parts[1].trimmingCharacters(in: .whitespaces)) : nil
                }.first ?? 0
                let have = buffer.count - end.upperBound
                if have >= length {
                    guard lines.first?.hasPrefix("POST /relay ") == true else {
                        respond(404, Data("POST /relay only\n".utf8))
                        return
                    }
                    let body = buffer[end.upperBound..<(end.upperBound + length)]
                    queue.async {
                        let (status, out) = exchange(Data(body))
                        respond(status, out)
                    }
                    return
                }
            }
            if complete || error != nil { c.cancel(); return }
            read()
        }
    }
    c.start(queue: queue)
    read()
}

let parameters = NWParameters.tcp
parameters.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!)
parameters.allowLocalEndpointReuse = true
let listener: NWListener
do {
    listener = try NWListener(using: parameters)
} catch {
    fail("cannot listen on 127.0.0.1:\(port): \(error.localizedDescription)")
}
listener.newConnectionHandler = serve
listener.stateUpdateHandler = { state in
    switch state {
    case .ready: say("serving the watch on http://127.0.0.1:\(port)/relay")
    case .failed(let e): fail("listening on 127.0.0.1:\(port) failed: \(e.localizedDescription)")
    default: break
    }
}
listener.start(queue: queue)

var lastLink: Int32 = -1
let pump = DispatchSource.makeTimerSource(queue: queue)
pump.schedule(deadline: .now(), repeating: .milliseconds(100))
pump.setEventHandler {
    var problem: UnsafeMutablePointer<CChar>?
    if nr_host_pump(relay, &problem) == 0 {
        say(problem.map { String(cString: $0) } ?? "the relay's state could not be written")
        nr_free(problem)
    }
    let now = nr_host_link(relay)
    if now != lastLink {
        say(["link: connecting", "link: connected", "link: lost"][Int(now)])
        lastLink = now
    }
}
pump.resume()
dispatchMain()
