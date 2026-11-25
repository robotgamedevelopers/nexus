# Talk to your AIs: the iPhone's form and voice

On an iPhone, nexus opens on a list of your AIs, and in an AI's tab the main input is your voice. Speech is recognized
on the phone, and what you say is sent as an ordinary prompt. The reply comes back as text, as speech, or as both.
The app stays a thin client (AGENTS.md): the server sees the same prompts as from a keyboard and decides everything.

## The form

The client policy chooses the form by the device's idiom (`ui.idiom`: `phone`, `pad` or `desktop`), in
`policy/rules/client/window.star` (`_phone`). Nothing about the form is hardcoded in C++.

- **The menu.** The app opens on a vertical list with one row per tab, in the strip's order and with the strip's
  titles (panel `menu`). There is no tab strip on the phone (`show_tabs` false). Tapping a row opens that tab full
  screen.
- **A tab.** The tab shows a way back (panel `back`, reading "< AIs"), the transcript, the account panel, the
  microphone (panel `voice`) and the prompt. The status panel follows if the window has a link status.
- **Typing.** Typing stays available, but the prompt does not take the focus on its own (`focus_prompt` false):
  focusing it brings up the on-screen keyboard over the microphone. A tap on the prompt focuses it.
- **Voice alone.** When replies are voice only, the transcript gives its room to a spacer.

On an iPad or the desktop the window is unchanged: it keeps the strip and has no menu or microphone. The panels are
listed in [policy.md](policy.md#the-iphones-form).

## A voice turn

Tap **Talk**. The first time, iOS asks for speech recognition and the microphone. While you speak, the transcript
forms above the button. Tap **Done** when you finish and the prompt is sent (a listen also ends by itself after 55
seconds):

- **Name an AI first** ("Emma, how's the sprint?") and the prompt goes to that AI's tab, with the name taken off:
  Emma gets "how's the sprint?". The app opens that tab. A greeting in front of the name is fine ("hey Emma, ...").
  So is the name at the end after a comma ("what's left, Emma?").
- **Name no one** and the prompt goes to the AI showing.
- **Say only a name** ("Rowan.") and that AI's tab opens with nothing sent.
- The line under the transcript says where the prompt went ("To Emma"). If a name fits two AIs, nothing is sent:
  the words go into the draft and the line asks which one. The same happens when no AI is showing. The menu is
  always the fallback.

Names are matched on the phone against the AIs the server lists, in `src/voice/name_match.h`. Matching ignores case,
punctuation and accents ("Zhi Yǎ Wén" matches "zhi ya wen"). It allows one letter of difference in a name of four or
more letters followed by a comma, to cover small recognizer slips. A fork adds spoken forms of a name in the client
config, for example `"phone": {"voice_names": {"zhi": ["zhi ya wen", "zhiya"]}}`.

The reply choice sits under the button. **Text** keeps replies silent. **Voice** speaks each reply and hides the
transcript. **Both** shows and speaks. Only replies to a prompt sent by voice are spoken. They are spoken in order,
and tapping Talk again stops the speech at once, so the recognizer never hears the phone. The first choice comes from
`"phone": {"reply": "voice"}` and the default is both. `"phone": {"voice": false}` turns the microphone off.

What is said never leaves the phone. Recognition is on the device only (`requiresOnDeviceRecognition`). If the phone
has no on-device model for its language, the app refuses and says how to get one; it never falls back to Apple's
servers. A refused permission, a missing microphone, an interrupted recording and a recognizer error each show one
plain line saying what happened and how to fix it. Whatever was heard before a failure is kept in the draft.

## How it is built

| Part | What it is |
| --- | --- |
| `src/voice/name_match.h` | Which AI a transcript is for, and the prompt that is left. Pure C++, no OS. |
| `src/voice/voice_turn.h` | One voice turn as a state machine: idle, asking, listening, finishing, denied. It decides, and returns acts (ask, listen, stop, speak, open, send, draft). Pure C++, and time is an argument. |
| `src/voice/voice_host.h` | Drains the OS's answers into the turn once a frame and carries out its acts through a `Speech` and a `VoiceSurface` (open, send, draft). Also `speakable()`, which turns a Markdown reply into speech. |
| `src/voice/speech.h` | The seam to the OS: `Speech` (ask, listen, stop, speak, quiet) and the thread-safe `SpeechInbox`. |
| `src/voice/speech_apple.mm` | Apple's implementation: SFSpeechRecognizer, AVAudioEngine, AVSpeechSynthesizer and AVAudioSession, run on a serial queue of its own so no frame waits on audio. |
| `src/voice_window.h` | The iPhone window's glue: it feeds the host the AI showing and the AIs' tabs, sends through `View::submit` like a typed prompt, and feeds back each reply as its blocks close. |

The voice turn has a TLA+ model, `models/VoiceTurn.tla`. Its invariants are that the microphone never listens over the
phone's speech, that text mode is silent, that a denied turn never listens, and that a turn never stalls. Its action
properties are that only answers to voice are spoken, each prompt is sent once, and a failure loses nothing. Its
liveness property is that every listen ends. Six reversed witnesses (`models/witnesses/WVoice*`) each break one
property and must be rejected. `tests/voice/voice_walk.cpp` walks the real `VoiceTurn` with random events and writes
its trace, and `models/VoiceTurnTrace.tla` checks that each walk is a behaviour of the spec. A walk with a faulty turn
end must be rejected. These run as `ctest -L model` (`tlc_voice_*`).

Tests:

- `name_match_unit`, `voice_turn_unit` and `voice_host_unit` (`-L unit`): pure logic over a fake `Speech`.
- `phone_policy_unit` (`-L unit`): the shipped client policy's phone form through `WindowApplication`.
- `ios_voice_named`, `ios_voice_current` and `ios_voice_planted` (`-L speech`, simulator builds): recorded speech in
  `tests/ios/audio` goes through the real on-device recognizer and the matcher in the app (`nexus --recognize AUDIO
  --ais NAMES`). They are not labelled `ios` because a CI runner cannot allow speech recognition. On a simulator, allow
  it once (launch the app, tap Talk, Allow), then run `ctest -L speech`.

To try voice in the simulator without a microphone, launch the app with `--voice-file AUDIO`. Talk then recognizes
that file instead of the microphone.

## A watch

A native watchOS client reuses everything except the recognizer. `src/voice` (the matcher, the turn and the host)
needs no Prax, no window and no OS, so a watch app links `nexus_voice` as it is. It gives a `VoiceHost` a
`VoiceSurface` of its own: `send` posts the prompt over its link to the server, `open` shows that AI, and `draft`
keeps the words for a tap. On each frame or OS callback it calls `frame(now, current, ais)` with the AIs the server
lists, and it shows `turn().line()` and `turn().heardText()`. The model, its trace check and every unit test then
cover the watch's turn unchanged, because it is the same `VoiceTurn`.

The one new part is a `Speech` for the watch. The watchOS SDK has no Speech framework (WatchOS 27.2 SDK: AVFAudio
and AVFoundation, no Speech.framework), so `speech_apple.mm` does not build there as it is. The watch's `Speech` would
take the transcript from the system's own dictation and post it to the `SpeechInbox` as one final `Heard`. Its `speak`
and `stopSpeaking` would be the same AVSpeechSynthesizer code as `speech_apple.mm`, which watchOS has. Only the
listening half is new; the turn, the routing, the confirmation line and the spoken replies are shared.
