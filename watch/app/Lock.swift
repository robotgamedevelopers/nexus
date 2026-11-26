// The owner's check (docs/watch.md): talking to AIs is only for the watch's owner, so nexus listens, and reads a reply,
// only while the watch is unlocked and on the wrist since its passcode was entered. LocalAuthentication's
// wrist-detection policy says exactly that: it passes without asking when the passcode was entered and the watch has
// stayed on the wrist since, and asks for the passcode otherwise. With no passcode set it fails: nexus stays locked.

import Foundation
import LocalAuthentication

enum OwnerCheck {
    enum Outcome {
        case passed
        case refused(String)    // why, in words for the person
    }

    static func run() async -> Outcome {
        let context = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthenticationWithWristDetection, error: &error) else {
            return .refused(why(error))
        }
        do {
            let ok = try await context.evaluatePolicy(.deviceOwnerAuthenticationWithWristDetection,
                                                      localizedReason: "Talk to your AIs")
            return ok ? .passed : .refused("the watch's owner was not confirmed")
        } catch {
            return .refused(why(error as NSError))
        }
    }

    private static func why(_ error: NSError?) -> String {
        guard let error else { return "the watch could not confirm its owner" }
        switch LAError.Code(rawValue: error.code) {
        case .passcodeNotSet: return "this watch has no passcode. Set one: nexus talks only to the watch's owner"
        case .userCancel, .appCancel, .systemCancel: return "the passcode was not entered"
        default: return "the watch could not confirm its owner: \(error.localizedDescription)"
        }
    }
}
