# Talk to your AIs: the iPhone's form and voice

On an iPhone, nexus opens on a menu of your AIs with a microphone at the bottom. Speak there and what you say goes to
the AI you name, and its reply comes back on the menu, spoken. In an AI's own page you type, or dictate into the
prompt. Speech is recognized on the phone. The app stays a thin client (AGENTS.md): the server sees the same prompts
as from a keyboard and decides everything.

## The form

The client policy chooses the form by the device's idiom (`ui.idiom`: `phone`, `pad` or `desktop`), in
`policy/rules/client/window.star` (`_phone`). Nothing about the form is hardcoded in C++. There is no tab strip on the
phone (`show_tabs` false).

**The menu** (panel `menu`, `src/view.cpp` `phoneMenu`) is where the app opens:

- One row per tab, titled as the Mac titles it (`tab_titles`). The order is the policy's (`menu_tabs`): by kind, in
  the order of the phone setting `menu_kinds` (by default the dashboard first, then the AI tabs, then terminals), each
  kind in the strip's order. Tapping a row opens that tab full screen.
- After the tabs, the policy's own rows (`menu_rows`, at most 8), each running a window command that takes no argument
  exactly as its keyboard shortcut does: for example a "New terminal" row running `open_terminal`, which is what
  Command-T does. None by default. The shapes and refusals are in [policy.md](policy.md#the-iphones-form).
- The rows sit centred vertically while they fit. When they do not, the list scrolls under the finger as a
  UIScrollView does (`src/finger_scroll.h`, `FingerScroll`): it follows a drag, a flick carries on and slows with
  UIScrollView's normal deceleration (0.998 of the speed kept each millisecond), and past either end it rubber-bands
  and springs back. A press that moves more than 10 pixels is a drag and never taps a row; a press on a moving list
  stops it and is no tap either.
- The microphone (**Talk**) sits at the bottom, above the safe area, in reach of the thumb. The list has its own room
  above it and never scrolls under it.

**An AI's page** has only a back chevron (an icon, no words) to the menu (panel `back`), the AI's title centred on the
screen's width whatever the chevron's (laid over it, never pushed by it, and shortened with an ellipsis when it does
not fit the same room on both sides), the transcript, and the prompt: one rounded box with a microphone icon inside at
its right end. The window takes exactly the room between the safe area (status bar, home indicator) and, when it
shows, the keyboard, so the prompt sits just above the home indicator or the keyboard. The account and status
panels show when the policy lists them, as on the desktop. The prompt does not take the focus on its own
(`focus_prompt` false); a tap on it brings up the keyboard.

On an iPad or the desktop the window is unchanged: it keeps the strip and has no menu or microphone.

## Talking on the menu

Tap **Talk**. The first time, iOS asks for speech recognition and the microphone (the button reads **Allow…** while
it asks). While you speak, the words form above the button. Tap **Done** when you finish and the prompt is sent (a
listen also ends by itself after 55 seconds):

- **Name an AI** ("Emma, how's the sprint?") and the prompt goes to that AI, with the name taken off: Emma gets
  "how's the sprint?". A greeting in front of the name is fine ("hey Emma, ..."). So is the name at the end after a
  comma ("what's left, Emma?").
- **Name no one** and the prompt goes to the default AI: the first AI in the list. Never the dashboard, never a
  terminal.
- **No AIs at all** (the server has not sent any yet): nothing is sent, and the line under the button says so.
- **A name that fits two AIs**: nothing is sent; the line asks which one and the words go to a draft.
- **Only a name** ("Rowan."): nothing is sent; the line shows the name.

The menu stays up. It shows who the prompt went to ("To Emma"), the end of the reply as it arrives (the last 280
bytes or so; the whole reply is in the AI's page), and an **Open Emma** button into that AI's page. Replies to a
prompt spoken on the menu are spoken aloud; in an AI's page they come back as text. This is fixed, not a setting
(`voice::replyOn`). They are spoken in order, and tapping Talk (it reads
**Talk over** while the phone speaks) stops the speech at once, so the recognizer never hears the phone.

Names are matched on the phone against the AIs the server lists, in `src/voice/name_match.h`. Matching ignores case,
punctuation and accents ("Zhi Yǎ Wén" matches "zhi ya wen"). It allows one letter of difference in a name of four or
more letters followed by a comma, to cover small recognizer slips. A fork adds spoken forms of a name in the client
config, for example `"phone": {"voice_names": {"zhi": ["zhi ya wen", "zhiya"]}}`.

## Dictating in an AI's page

The microphone in the prompt is plain dictation: the words go into the prompt as they form, after whatever was typed
there, and the person edits and sends them as a typed prompt. Nothing is sent from the microphone, no name is matched,
and nothing is spoken; replies in a tab come back as text. Tap the icon again to stop.

`"phone": {"voice": false}` in the client config turns both microphones off.

What is said never leaves the phone. Recognition is on the device only (`requiresOnDeviceRecognition`). If the phone
has no on-device model for its language, the app refuses and says how to get one; it never falls back to Apple's
servers. A refused permission, a missing microphone, an interrupted recording and a recognizer error each show one
plain line saying what happened and how to fix it. Whatever was heard before a failure is kept in a draft.

## How it is built

| Part | What it is |
| --- | --- |
| `src/voice/name_match.h` | Which AI a transcript is for, and the prompt that is left. Pure C++, no OS. |
| `src/voice/voice_turn.h` | One voice turn as a state machine: idle, asking, listening, finishing, denied. It decides, and returns acts (ask, listen, stop, speak, open, send, draft). With no AI showing, a prompt naming no one goes to the first AI. Pure C++, and time is an argument. |
| `src/voice/voice_host.h` | Drains the OS's answers into the turn once a frame and carries out its acts through a `Speech` and a `VoiceSurface` (open, send, draft). Also `speakable()`, which turns a Markdown reply into speech. |
| `src/voice/speech.h` | The seam to the OS: `Speech` (ask, listen, stop, speak, quiet) and the thread-safe `SpeechInbox`. |
| `src/voice/speech_apple.mm` | Apple's implementation: SFSpeechRecognizer, AVAudioEngine, AVSpeechSynthesizer and AVAudioSession, run on a serial queue of its own so no frame waits on audio. |
| `src/voice_window.h` | The iPhone window's glue. On the menu it gives the turn the AIs (with `voice_names`) and no AI showing, sends through `View::submit` like a typed prompt, and collects the reply for the menu. In a tab it gives no AIs, and turns what is heard into dictation in that tab's prompt. |
| `src/finger_scroll.h` | The menu's scrolling under the finger: rest, track, drag, fling, spring. Pure C++, and time is an argument. |

The voice turn has a TLA+ model, `models/VoiceTurn.tla`. Its invariants are that the microphone never listens over the
phone's speech, that text mode is silent, that a denied turn never listens, and that a turn never stalls. Its action
properties are that only answers to voice are spoken, each prompt is sent once, a failure loses nothing, and a prompt
naming no one goes to the AI showing or, with none showing, to the first AI (`DefaultIsFirst`, with the constant
`First`). Its liveness property is that every listen ends. Seven reversed witnesses (`models/witnesses/WVoice*`,
`WVoiceNoDefault` among them) each break one property and must be rejected. `tests/voice/voice_walk.cpp` walks the
real `VoiceTurn` with random events and writes its trace, and `models/VoiceTurnTrace.tla` checks that each walk is a
behaviour of the spec. A walk with a faulty turn end must be rejected. These run as `ctest -L model` (`tlc_voice_*`).

Tests:

- `name_match_unit`, `voice_turn_unit` and `voice_host_unit` (`-L unit`): pure logic over a fake `Speech`.
- `finger_scroll_unit` (`-L unit`): drag, fling, the ends and taps, on the test's own times.
- `phone_policy_unit` (`-L unit`): the shipped client policy's phone form through `WindowApplication`.
- `menu_rows_unit` (`-L unit`): `menu_rows` read, run as shortcuts, and each malformed form refused with its reason.
- `ios_voice_named`, `ios_voice_current` and `ios_voice_planted` (`-L speech`, simulator builds): recorded speech in
  `tests/ios/audio` goes through the real on-device recognizer and the matcher in the app (`nexus --recognize AUDIO
  --ais NAMES`). A CI runner cannot allow speech recognition, so they are registered only in a simulator build
  configured with `-DNEXUS_IOS_SPEECH_TESTS=ON`. On that simulator, allow it once (launch the app, tap Talk, Allow),
  then run `ctest -L speech`.

To try voice in the simulator without a microphone, launch the app with `--voice-file AUDIO`. Talk then recognizes
that file instead of the microphone.

## A watch

A native watchOS client reuses everything except the recognizer. `src/voice` (the matcher, the turn and the host)
needs no Prax, no window and no OS, so a watch app links `nexus_voice` as it is. It gives a `VoiceHost` a
`VoiceSurface` of its own: `send` posts the prompt over its link to the server, `open` shows that AI, and `draft`
keeps the words for a tap. On each frame or OS callback it calls `frame(now, current, ais)` with the AIs the server
lists (and no AI showing, as the phone's menu does, so a prompt naming no one goes to the first AI), and it shows
`turn().line()` and `turn().heardText()`. The model, its trace check and every unit test then cover the watch's turn
unchanged, because it is the same `VoiceTurn`.

The one new part is a `Speech` for the watch. The watchOS SDK has no Speech framework (WatchOS 27.2 SDK: AVFAudio
and AVFoundation, no Speech.framework), so `speech_apple.mm` does not build there as it is. The watch's `Speech` would
take the transcript from the system's own dictation and post it to the `SpeechInbox` as one final `Heard`. Its `speak`
and `stopSpeaking` would be the same AVSpeechSynthesizer code as `speech_apple.mm`, which watchOS has. Only the
listening half is new; the turn, the routing, the confirmation line and the spoken replies are shared.
