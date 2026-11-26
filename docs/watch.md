# nexus on Apple Watch

Talk to your AIs, and they talk back. That is all the watch does.

Hold the button and speak. Start with an AI's name to talk to that AI: "Hey Rowan, is main green?" or "Emma, what's on
the board?". With no name, the words go to the first AI in the server's list. The watch says who it sent the words to
("To emma."), then speaks the reply. The screen shows only who is talking, and what went wrong when something did.

There are no tabs, dashboard, terminals, settings or transcript on the watch.

The screen never flickers. The core decides its words (`watch/core/screen.h`), and a fresh launch settles at once:
until the phone has reached nexus-server, or for at most four seconds after launch, the watch shows its idle screen
rather than flashing "No server" while the phone's link starts. A link that stays down past that, or is lost after it
was reached, shows at once.

## Switching an AI's account

The AI's name is at the top of the screen: the AI talked to last, or else the first in the server's list (the AI that
words with no name go to). A relaunch shows the last launch's first AI from its first frame, until the phone sends the
list. Tap it to
see that AI's signed-in accounts. The Digital Crown scrolls the list, and the account the AI is on now is marked. Each
row shows the account's email, or else its label, or else its name. Accounts that need signing in are not listed: sign
them in with nexus on the iPhone first.

Tap another account to switch the AI to it. The watch says "Switching emma to ...", and the row is marked as switching
until the server has it. Tapping the account the AI is on already does nothing. One switch is under way at a time: a
switch for another AI waits until the first is done, and the watch says so.

The switch goes through the phone, like the words do. The phone hands it to the AI's session as a SetAccount, the same
action the iPad and Mac windows send. The phone checks the account against the server's list first; if it has been
signed out since, the switch is refused and the watch says why. The watch holds its choice, across relaunches, until
the phone says the server has it, so a switch is never dropped silently. The list always shows the account the server
says the AI is on, never the watch's own wish.

**This is a sketch.** The watch app, its core and its models run, in the simulator against a Mac standing in for the
phone. The iPhone's side of the relay is not built, so on a real watch the app cannot reach a server yet, and the
phone's lock described below is not enforced anywhere yet: it is the design, and the Mac's stand-in only imitates it
(`--locked`). See "Not yet" at the end.

## Only for the watch's owner

Talking to AIs is powerful, so only the watch's owner can do it.

- **The watch.** nexus listens and reads replies only while the watch is unlocked and on the wrist since its passcode
  was entered. It checks with LocalAuthentication's wrist-detection policy each time the app comes forward and at each
  press. When the app goes to the background (locked, taken off the wrist, or another app), the watch locks. While
  locked, nexus will not listen. A reply being read stops and is neither spoken nor shown. The watch says each of these
  aloud and on screen. The reply waits until the watch is unlocked, then it is read. A watch with no passcode stays
  locked and says why.
- **The phone (not enforced yet: the iPhone relay is not built).** The watch reaches nexus-server through the nexus app
  on the paired iPhone. That app's relay is to refuse the watch until the phone app's own lock has been passed since
  the phone last slept. Until then it takes no words,
  passes nothing to the server and sends nothing back. It answers "nexus on your iPhone is locked. Open it on the phone
  and unlock it.", which the watch speaks and shows.

## How it reaches the server

The watch is a thin client. The server decides everything. The watch does three things itself:

- **Names.** It matches the AI's name against the list the server sends. The matcher is the iPhone's
  (`src/voice/name_match.h`), so the phone and the watch send the same words to the same AI.
- **Speech out.** The reply is spoken with `AVSpeechSynthesizer`.
- **Recording.** It records what you say.

The words in the recording are recognized on the phone, by `SpeechTranscriber`, because watchOS has no Speech
framework.

### Why the watch goes through the iPhone

The watch does not run nexus's link (mosh's UDP transport) itself. Apple's TN3135, "Low-level networking on watchOS",
says BSD sockets do not work for networking on watchOS under any circumstances.

The Network framework is allowed only in three cases: while streaming audio, during a CallKit call, or for a
DeviceDiscoveryUI listener. Outside those cases, a connection waits with `ENETDOWN`. URLSession's HTTP is always allowed.

nexus's link has two problems on the watch:

- Its transport is BSD sockets, and mosh stays unmodified.
- It starts by running `ssh <host> nexus-server new`, and a watch cannot start a process.

The simulator allows low-level networking freely, so it would not catch either problem.

So the watch sends small request and response frames through the iPhone over WatchConnectivity (`sendMessageData`).
The nexus iPhone app runs the relay over its own link to the server.

### The two state machines

**The voice turn** (`watch/core/voice_turn.h`, `watch/models/VoiceTurn.tla`) has these phases: listening,
transcribing, choosing, sending, waiting, speaking, interrupted, and disconnected. The owner's lock sits across all of
them.

**The relay** (`watch/core/relay.h`, `watch/models/Relay.tla`) delivers each prompt to the server at most once, through
restarts of the watch app, the phone app, or both, and never loses one silently:

- The watch numbers its prompts and keeps them until the phone says the server has them.
- The phone durably notes each prompt it hands to the link before it hands it over.
- After a restart, the phone tells the watch which numbers it can no longer vouch for. The watch says those prompts
  may not have arrived.

**The account switch** (`watch/models/AccountSwitch.tla`) rides the same frames: the watch numbers its choice and
holds the newest until the phone says the server has it, and a phone that restarts takes the choice again. Only
signed-in accounts are ever chosen, a choice always changes something, and a choice leaves the watch unapplied only for
a newer one. The model does not claim that the server ends on the newest choice: an earlier run of the phone's link
can still deliver an older one late.

The models are checked by TLC. The code is bound to them: seeded random walks of the real code are checked as
behaviours of the specs. Each model has reversed witnesses that must fail, and planted faults that must be rejected.

## Building

The core alone (logic only; no Prax, no link), with its unit tests:

    cmake -S watch -B build-watch -G Ninja && cmake --build build-watch && ctest --test-dir build-watch -L unit

The app for the simulator, or for a watch. CMake builds Swift with the Ninja and Xcode generators only. A device build
is signed with `-DNEXUS_WATCH_TEAM=<identity>`, or ad hoc without one, which no watch installs.

    cmake -S . -B build-watch-sim -G Ninja -DCMAKE_SYSTEM_NAME=watchOS -DCMAKE_OSX_SYSROOT=watchsimulator \
          -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_BUILD_TYPE=Release
    cmake --build build-watch-sim        # build-watch-sim/watch/nexus.app

    cmake -S . -B build-watch-device -G Ninja -DCMAKE_SYSTEM_NAME=watchOS -DCMAKE_OSX_SYSROOT=watchos \
          -DCMAKE_OSX_ARCHITECTURES=arm64_32 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_BUILD_TYPE=Release

## Tests

- **Per change** (`ci.yml`, job `watch`): the core's unit tests, and the app built warning-free for the simulator and
  for a watch. The still-frames test is among the unit tests (`watch_unit_screen`): a fresh launch's frames are all
  its first until the phone reaches the server. They run only there, in the watch's own build (`cmake -S watch`):
  nexus's own build leaves the watch out of its default targets.
- **The models**, in a nightly shard of their own (`nightly.yml`, shard `watch-app`):
  - TLC on `VoiceTurn.tla`, `Relay.tla` and `AccountSwitch.tla`.
  - Every reversed witness under `watch/models/witnesses`.
  - The code's walks over four seeds.
  - Each planted fault, which must be rejected.
- **Speech**, nightly (`nightly.yml`, job `watch-speech`; `watch/tests/speech.cmake`):
  - `say` speaks recordings, which go through the phone's recognizer and the name matcher.
  - Silence must be refused with its reason.
  - A planted twin of the test must fail.

### The simulator's end to end

`nexus-watch-relay` (`watch/host/main.swift`) runs the phone's relay and recognizer on a Mac, in place of the iPhone. It
speaks HTTP on 127.0.0.1, which the simulator's app reaches with `--relay`. The run:

1. Run `nexus-server serve` with a home of its own, and a fake driver for each AI.
2. Build it (`cmake --build build --target nexus_watch_relay_host`: it is not in the default build, nor are the
   model walks and the speech test's matcher, so a pull request's macOS build stays inside its 2 minutes), then run
   `nexus-watch-relay --port <n> --state <file> -- env HOME=<home> nexus-server new --dir <dir>`.
3. Build the app for the simulator with `-DCMAKE_BUILD_TYPE=Debug`, install it on a watch simulator, and launch it with
   these test options:
   - `--relay http://127.0.0.1:<n>/relay`
   - `--speak-file` with recordings made by `say`, in place of the microphone
   - `--voice-to <dir>`, so what is said goes to files rather than the speaker
   - `--log <file>`
   - `--drive "unlock;wait:2;press;wait:2;release;..."` (`accounts` opens the account list)

In the simulator, the owner's check cannot pass because there is no wrist. A driven run plays the wrist itself with
`lock` and `unlock`. The test options and the driven wrist are compiled only into a Debug build for the simulator
(`NEXUS_WATCH_TEST_OPTIONS`, `cmake/watchos.cmake`). A Release build, and every build for a watch, has none of them:
CI checks the Release binaries with `watch/tests/no_test_options.cmake`.

To check the run, transcribe the spoken files with `nexus-watch-relay --transcribe`.

To see the account list without a relay or a server, launch with `--canned-phone watch/tests/data/accounts.phoneframe`
and `--drive accounts`. Every frame is then answered with that phone frame: one AI, emma, with three signed-in
accounts and one that needs signing in.

To see a refusal:

- **A locked phone:** start the relay with `--locked`.
- **No server:** give the relay an unreachable `--host`.
- **No microphone:** revoke it with `simctl privacy`.

## Not yet

- **The iPhone's side of the relay.** This is WatchConnectivity into the nexus iPhone app, with its link and its
  recognizer. It waits on the link on iOS. Until it lands, the watch app's phone carrier reports that the iPhone is not
  reachable, and the phone's lock is not enforced: only the Mac's stand-in imitates it.
- **The account switch on the iPhone.** The relay carries it both ways, and the Mac's stand-in hands it to the server
  (`watch/host/relay_host.cpp`). On the iPhone it waits on the same missing relay.
- **A real watch.** The app has not yet been run on a real watch.
