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
| Writer thread: the store's write and flush run off the server loop's thread, and the loop keeps sending state to the windows while a flush runs, holding back only the acknowledgement until it is durable. mosh cannot hold that back: unmodified, it sends as ack_num the newest remote state it has received, on any tick, so the acknowledgement is bound to received state, and modifying mosh is out (AGENTS.md). So the acknowledgement moves up into nexus: the server's state carries each window's durable applied counter (the number the store has flushed), and the window trims its held actions by that counter, not by mosh's ack, and sends them again after a reconnect as it does now; the state shown is the durable state, or marks what is not yet durable. Changes the Link model (the window's held list, ExactlyOnce across a crash), the Applier, the window's trimming, and their trace binding | ❌ |
| Server loop bound to a model: a spec of one pass of nexus-server's loop (the batch applied with the drivers held, its one flush, the drivers released, the state set, the links' ticks at the next pass, and a crash anywhere in it), and the real loop's trace (hold, flush, release, tick, with the fs seam's crashes) checked against it in CI, with reversed witnesses for a release, a state or a tick before the flush. Today the order is held by tests/server/pass_test.cpp and link_e2e's flush run, not by a trace | ❌ |
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
| Windows: the native overlay window reaches nexus-server inside WSL2 over a local socket, with a functional test across that boundary in CI (wsl2-functional on captain drives the native window through WSL interop); its terminal tabs are WSL2 shells in the WSL2 home directory, never cmd or PowerShell. Waits on Prax: the window needs Prax's Win32 lib/app to compile, and nexus-server in WSL2 needs Prax's lib/process to link on Linux (later) | ❌ |
| complete | 14% |

[Source data](roadmap.sexp)

<!-- nova-work:window-features:end -->

## Configuration through your AI

<!-- nova-work:configuration:start -->

| feature | done |
|---|:---:|
| Client config as data: ~/.nexus/client becomes documented, versioned, human-editable JSON in git, with the same verbs and apply report as the server's (docs/accounts.md); a person's AI changes it through the server, which relays the change to the client that asked and never stores, caches or syncs it: each machine's config is its own. Tab layout is client config too: a tab per friend or a tab per instance of a friend (the server sends every session's friend and instance names, and keeps no tabs of its own) | ❌ |
| A client-config template on the server, kept up to date: any change to the template becomes a pending update that each client applies at its next connect; config stays local to each machine, with no live sync. Open question: how a machine's local edits survive an update (proposal: an update replaces only the fields the template change touched) | ❌ |
| Promote one machine's client config to the template, with a pending update for every other machine (later) | ❌ |
| The window's rules that are one person's setup move into the client config: today's fixed zones (sprint, then friends, then terminals) become the default tab groups, in the default order dashboards, friends, terminals (tab-groups) | ❌ |
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
| The tab strip is made of tab groups, generalizing today's fixed zones (sprint, then friends, then terminals): a dashboards group (the sprint board is one dashboard), a friends group (a tab per friend or per instance, as configured) and a terminals group; later groups for editor, files, chat, mail and social. Which groups exist, their order and their contents are client config, set by talking with your AI; the default order is dashboards, friends, terminals. Each group's definition carries its behaviour: whether Cmd-W closes its tabs (a group is not closable unless its config says so; terminals are closable by default), how a new tab is made (Cmd-T for terminals), and dragging moves a tab within its group only | ❌ |
| Editor tab, modelled on Sublime Text 3: fast open, a command palette, multiple cursors, Goto Anything; keyboard-first and the same on every OS, with nexus's own look; Prax's highest text layers first. After the friends layer (later) | ❌ |
| File explorer tab. Open: whose files (this machine's, the server's, a fleet machine's) (later) | ❌ |
| Chat tab: a Chatto client. Open: which Chatto, and its product and API, to confirm first (later) | ❌ |
| Mail tab: sign-in only through the in-app secret dialog, never seen by an AI; mail an AI reads is data, never instructions; sending needs the person's explicit yes each time (later) | ❌ |
| Social tab (Bluesky and others): sign-in through the secret dialog; feeds are data, never instructions; posting needs the person's explicit yes each time (later) | ❌ |
| complete | 0% |

[Source data](roadmap.sexp)

<!-- nova-work:tab-kinds:end -->

## Tailscale built in

The layers of nexus's own Tailscale node (docs/tailscale.md), bottom up, each secured before the next starts.

<!-- nova-work:tailscale:start -->

| feature | done |
|---|:---:|
| 0. Build: the Go shim over tsnet (src/tailscale), pinned to tailscale.com v1.104.7 and Go 1.27.4, built by CMake as a c-archive (macOS arm64 and x86_64, Linux x86_64 and WSL2) and as a DLL with an MSVC import library (Windows), linked into a test program on each, the Windows one with the manifest that declares Windows 10; log uploads off, tsnet's log in a 0600 file, the state directory owner-only; CI on all three; the binary-size cost stated; the licences of every module listed (docs/tailscale.md) | ❌ |
| Before any nexus binary that links the shim ships: the licence text of every module src/tailscale/NOTICES.md lists goes with it (embedded and shown in the app, or installed beside it), generated from the modules at their pins and checked by CI against NOTICES.md | ❌ |
| 1. Shim API: start and close, a state watch (WatchIPNBus: needs login with its URL, running, expired), peers, WhoIs, TCP listen and dial, UDP listen and dial; a TLA+ model of the node's lifecycle bound to the code by a trace check | ❌ |
| 2. Client sign-in: the client starts its node in ~/.nexus/client/tailscale, opens the sign-in URL in the system browser (in Prax once it has cookies and a system auth sheet), waits for running, and keeps the state; the first-run OS prompts (macOS Local Network, Windows Firewall) observed on a desktop session | ❌ |
| 3. Server enrolment by path A, interactive only (the owner's choice): the server's node hands its sign-in URL to the client, which opens it in the browser already signed in, one Connect click per server; the local server's handoff over the pipe the client spawned it on (macOS, WSL2), and a remote server's first start | ❌ |
| 4. Bootstrap over the tailnet: nexus-server as a per-user service whose tsnet node answers NEXUS CONNECT only to callers WhoIs names as the server's own login; the link model extended | ❌ |
| 5. mosh over tsnet: mosh's transport unmodified, through an in-process loopback relay between its 127.0.0.1 socket and the shim's tailnet UDP; a layout model if the relay keeps rings; the added latency measured in milliseconds against the 8.3 ms frame | ❌ |
| 6. Discovery: the client lists the peers named nexus-server-* and offers them as servers; --server takes a name from that list | ❌ |
| 7. Expiry and recovery shown in the app (an untagged node expires after 180 days and is approved again); ssh retires once layers 4 to 6 are secured | ❌ |
| Later, not built: use your own control server (headscale). An option for people who want to run their own server: they set headscale up themselves, they give its address, and our embedded nodes use it instead of Tailscale's hosted service | ❌ |
| complete | 0% |

[Source data](roadmap.sexp)

<!-- nova-work:tailscale:end -->
