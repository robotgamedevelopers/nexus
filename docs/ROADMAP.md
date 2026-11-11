# nexus roadmap

<!-- The tables between the nova-work markers are generated from roadmap.sexp by nova-roadmap: edit the data and run `nova-roadmap render`. Everything else on this page is written by hand. -->

## Layers

<!-- nova-work:layers:start -->

| feature | done |
|---|:---:|
| 1. Window: tabs, transcript and prompt draw and take input (in progress) | ❌ |
| 2. Server and sync: a server holds tabs, transcripts and drafts and keeps running without a client; the client reconnects from anywhere to the current state; nothing typed is lost or applied twice (in progress) | ❌ |
| 3. Friends: each tab is a real conversation with one friend, reached through a driver process that is a child of the server (one per tab, docs/driver-protocol.md); nothing the server received is lost or shown twice; a prompt reaches the harness at most once; a turn cut by a driver's exit is shown as cut | ❌ |
| 4. Heads-down: timer and queue live on the server; visible queue, stop always gets through, one away-report at the end | ❌ |
| 5. Fleet: see the machines and what runs on each | ❌ |
| 6. Sprint: see and deal cards without leaving the window | ❌ |
| 7. Panels: friends add UI elements when asked; a panel is data (what to show, which tool's output feeds it, what each button sends) rendered by nexus, never code loaded into it | ❌ |
| Later: one app, nexus-server, starts at boot on the always-on machine and runs every tool and every friend as its own child process, so permissions are granted once to that app and nothing else runs as a separate app or daemon | ❌ |
| iOS client (one universal app for iPhone and iPad), a second client of the server; after layer 2, waiting on Prax's iOS app backend (later) | ❌ |
| Install through AI, no installer: a person in their own AI, on their own subscription, points it at this repo; the AI builds nexus from source by README, installs nexus-server as a login item, and sets nexus up with one friend, its own harness and account. No sprint, no fleet. README is the installer and is proven by a cold run on a clean Mac. After the friends layer. (later) | ❌ |
| complete | 0% |

[Source data](roadmap.sexp)

<!-- nova-work:layers:end -->

## Window features

<!-- nova-work:window-features:start -->

| feature | done |
|---|:---:|
| Terminal tabs: a shell per tab, after the friends; added with Cmd-T, closed with Cmd-W | ✅ |
| Sprint tab: nova-sprint's board, always first and always Cmd-1; never moved, closed or typed into; the page opaque, as nova-sprint serves it | ❌ |
| The plus button uploads a picked file to the server, into the selected AI's incoming/ directory; an image is also posted to the chat (needs layers 2 and 3) (later) | ❌ |
| The upload's storage on the server: each file a blob in the store by its content hash, sent in chunks that resume after a cut, and placed in the AI's incoming/ in one atomic step; waits for the upload design (an earlier draft is in the git history of src/store) (later) | ❌ |
| Hotkeys to choose a friend's model and effort, in place of the model name that stood under the prompt (later) | ❌ |
| HDR and wide gamut: the window asks Prax for its HDR swap chain (half floats, extended sRGB, EDR, gamut-mapped to the display) once Prax's display pass keeps alpha for a see-through window, and once nexus draws colours past sRGB or brighter than SDR white (later) | ❌ |
| complete | 16% |

[Source data](roadmap.sexp)

<!-- nova-work:window-features:end -->
