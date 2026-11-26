// nexus on the wrist: talk to your AIs, and they talk back (docs/watch.md). Hold the button and say an AI's name and
// what to tell it ("Hey Rowan, ...", "Emma, ..."); with no name it goes to the first AI in the server's list. The
// reply is spoken. The screen shows only who is talking, and what went wrong when something did.

import SwiftUI

@main
struct NexusWatchApp: App {
    @StateObject private var client = Client(options: Options.fromArguments())
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView(client: client)
        }
        // In the background (the watch locked, or taken off the wrist, or another app): locked. In front again: the
        // owner's check again. Wrist down alone (inactive) changes nothing: a reply is still read to the end.
        .onChange(of: scenePhase) { _, phase in
            if phase == .active { client.scene(active: true) } else if phase == .background { client.scene(active: false) }
        }
    }
}

struct ContentView: View {
    @ObservedObject var client: Client
    @State private var holding = false

    // Who is talking, or what the watch is doing: the core decides the words (watch/core/screen.h), and a fresh launch
    // settles at once (no "No server" flash while the phone's link starts).
    var body: some View {
        VStack(spacing: 6) {
            Text(client.status)
                .font(client.phase == Int32(NW_SPEAKING) ? .title3 : .footnote)
                .foregroundStyle(client.phase == Int32(NW_LISTENING) ? Color.red : Color.primary)
                .lineLimit(2)
                .multilineTextAlignment(.center)
            if !client.problem.isEmpty {
                ScrollView {
                    Text(client.problem)
                        .font(.caption2)
                        .foregroundStyle(.orange)
                        .multilineTextAlignment(.center)
                }
            } else {
                Spacer(minLength: 0)
            }
            if client.phase == Int32(NW_INTERRUPTED) {
                HStack {
                    Button { client.resume() } label: { Image(systemName: "play.fill") }
                    Button { client.dismiss() } label: { Image(systemName: "xmark") }
                }
            } else {
                talkButton
            }
        }
    }

    private var talkButton: some View {
        Circle()
            .fill(client.phase == Int32(NW_LISTENING) ? Color.red : Color.blue)
            .frame(width: 64, height: 64)
            .overlay(Image(systemName: client.phase == Int32(NW_SPEAKING) ? "stop.fill" : "mic.fill")
                .font(.title2).foregroundStyle(.white))
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    if !holding { holding = true; client.press() }
                }
                .onEnded { _ in
                    holding = false
                    client.release()
                })
            .accessibilityLabel("Hold to talk")
    }
}
