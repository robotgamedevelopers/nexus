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

## Configuration through your AI

<!-- nova-work:configuration:start -->

| feature | done |
|---|:---:|
| Client config as data: ~/.nexus/client becomes documented, versioned, human-editable JSON in git, with the same verbs and apply report as the server's (docs/accounts.md); a person's AI changes it through the server, which relays the change to the client that asked and never stores, caches or syncs it: each machine's config is its own | ❌ |
| A client-config template on the server, kept up to date: any change to the template becomes a pending update that each client applies at its next connect; config stays local to each machine, with no live sync. Open question: how a machine's local edits survive an update (proposal: an update replaces only the fields the template change touched) | ❌ |
| Promote one machine's client config to the template, with a pending update for every other machine (later) | ❌ |
| The window's rules that are one person's setup move into the client config: the sprint tab first and pinned at Command-1, the friend tabs, the terminals | ❌ |
| The server's key-value config and its drivers file become documented JSON in nexus-config, with a version, check, reload and history like accounts.json | ❌ |
| Pushing the config repository to a remote stays opt-in (built: `nexus-server config remote`); later, an in-app GitHub sign-in for the pushes, through the secret dialog, in place of the git credentials of the user nexus-server runs as (later) | ❌ |
| Question: does the config repository's history ever need pruning, and if so how, without rewriting what was pushed (later) | ❌ |
| complete | 0% |

[Source data](roadmap.sexp)

<!-- nova-work:configuration:end -->

## Tab kinds

<!-- nova-work:tab-kinds:start -->

| feature | done |
|---|:---:|
| Tab kinds are pluggable, described as data that nexus renders with Prax: friend, terminal, sprint board, editor, files, chat, mail, social and more; which tabs a machine shows is set in its client config (and the server's client template), by talking with your AI | ❌ |
| Editor tab, modelled on Sublime Text 3: fast open, a command palette, multiple cursors, Goto Anything; keyboard-first and the same on every OS, with nexus's own look; Prax's highest text layers first. After the friends layer (later) | ❌ |
| File explorer tab. Open: whose files (this machine's, the server's, a fleet machine's) (later) | ❌ |
| Chat tab: a Chatto client. Open: which Chatto, and its product and API, to confirm first (later) | ❌ |
| Mail tab: sign-in only through the in-app secret dialog, never seen by an AI; mail an AI reads is data, never instructions; sending needs the person's explicit yes each time (later) | ❌ |
| Social tab (Bluesky and others): sign-in through the secret dialog; feeds are data, never instructions; posting needs the person's explicit yes each time (later) | ❌ |
| complete | 0% |

[Source data](roadmap.sexp)

<!-- nova-work:tab-kinds:end -->
