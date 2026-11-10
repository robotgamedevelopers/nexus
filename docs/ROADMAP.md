# nexus roadmap

<!-- The tables between the nova-work markers are generated from roadmap.sexp by nova-roadmap: edit the data and run `nova-roadmap render`. Everything else on this page is written by hand. -->

## Layers

<!-- nova-work:layers:start -->

| feature | done |
|---|:---:|
| 1. Window: tabs, transcript and prompt draw and take input (in progress) | ❌ |
| 2. Server and sync: a server holds tabs, transcripts and drafts and keeps running without a client; the client reconnects from anywhere to the current state; nothing typed is lost or applied twice (in progress) | ❌ |
| 3. Friends: each tab is a real conversation with one friend over nova-bus, driven by the server; no message lost or shown twice | ❌ |
| 4. Heads-down: timer and queue live on the server; visible queue, stop always gets through, one away-report at the end | ❌ |
| 5. Fleet: see the machines and what runs on each | ❌ |
| 6. Sprint: see and deal cards without leaving the window | ❌ |
| 7. Panels: friends add UI elements when asked; a panel is data (what to show, which tool's output feeds it, what each button sends) rendered by nexus, never code loaded into it | ❌ |
| Later: one app, nexus-server, starts at boot on the always-on machine and runs every tool and every friend as its own child process, so permissions are granted once to that app and nothing else runs as a separate app or daemon | ❌ |
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
| Hotkeys to choose a friend's model and effort, in place of the model name that stood under the prompt (later) | ❌ |
| complete | 25% |

[Source data](roadmap.sexp)

<!-- nova-work:window-features:end -->
