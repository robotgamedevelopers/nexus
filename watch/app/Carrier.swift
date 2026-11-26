// How a frame reaches the phone's relay and its answer comes back (watch/core/relay.h). On a watch: WatchConnectivity
// to the paired iPhone, whose nexus app holds the link to nexus-server (sendMessageData and its reply). In the
// simulator's end-to-end test (--relay <url>): HTTP to nexus-watch-relay on the Mac, which runs the same relay. A frame
// that gets no answer says why, in words for the person.

import Foundation
import WatchConnectivity

protocol Carrier: AnyObject {
    func exchange(_ frame: Data, answer: @escaping (Result<Data, CarrierError>) -> Void)
}

struct CarrierError: Error {
    let why: String
}

final class PhoneCarrier: NSObject, Carrier, WCSessionDelegate {
    private var session: WCSession? = WCSession.isSupported() ? WCSession.default : nil

    override init() {
        super.init()
        session?.delegate = self
        session?.activate()
    }

    func exchange(_ frame: Data, answer: @escaping (Result<Data, CarrierError>) -> Void) {
        guard let session else {
            answer(.failure(CarrierError(why: "this watch cannot talk to a phone")))
            return
        }
        guard session.activationState == .activated else {
            answer(.failure(CarrierError(why: "the link to your iPhone is still starting")))
            return
        }
        guard session.isReachable else {
            answer(.failure(CarrierError(why: "your iPhone is not reachable: open nexus on it, and keep it near")))
            return
        }
        session.sendMessageData(frame, replyHandler: { data in
            DispatchQueue.main.async { answer(.success(data)) }
        }, errorHandler: { error in
            DispatchQueue.main.async { answer(.failure(CarrierError(why: "your iPhone did not answer: \(error.localizedDescription)"))) }
        })
    }

    func session(_ s: WCSession, activationDidCompleteWith state: WCSessionActivationState, error: Error?) {}
}

final class LoopbackCarrier: Carrier {
    private let url: URL
    private let urlSession: URLSession

    init(url: URL) {
        self.url = url
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 5
        urlSession = URLSession(configuration: config)
    }

    func exchange(_ frame: Data, answer: @escaping (Result<Data, CarrierError>) -> Void) {
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.httpBody = frame
        urlSession.dataTask(with: request) { data, response, error in
            let result: Result<Data, CarrierError>
            if let error {
                result = .failure(CarrierError(why: "the relay at \(self.url.absoluteString) did not answer: \(error.localizedDescription)"))
            } else if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                let text = data.flatMap { String(data: $0, encoding: .utf8) } ?? ""
                result = .failure(CarrierError(why: "the relay refused the frame (\(http.statusCode)): \(text)"))
            } else {
                result = .success(data ?? Data())
            }
            DispatchQueue.main.async { answer(result) }
        }.resume()
    }
}

#if NEXUS_WATCH_TEST_OPTIONS && targetEnvironment(simulator)
// --canned-phone <file>: every frame is answered with the same phone frame, read from the file, with no relay or
// server behind it: the screens with a server's state in them, for a look (a Debug build for the simulator only).
final class CannedCarrier: Carrier {
    private let answer: Result<Data, CarrierError>

    init(file: URL) {
        if let data = try? Data(contentsOf: file) {
            answer = .success(data)
        } else {
            answer = .failure(CarrierError(why: "the canned phone frame \(file.path) cannot be read"))
        }
    }

    func exchange(_ frame: Data, answer reply: @escaping (Result<Data, CarrierError>) -> Void) {
        let a = answer
        DispatchQueue.main.async { reply(a) }
    }
}
#endif
