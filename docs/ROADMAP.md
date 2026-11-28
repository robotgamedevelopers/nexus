# nexus roadmap

<!-- The tables between the nova-work markers are generated from roadmap.sexp by nova-roadmap: edit the data and run `nova-roadmap render`. Everything else on this page is written by hand. -->

## Layers

<!-- nova-work:layers:start -->

| feature | done |
|---|:---:|
| 1. Window: tabs, transcript and prompt draw and take input (in progress) | ❌ |
| 2. Server and sync: a server holds tabs, transcripts and drafts and keeps running without a client; the client reconnects from anywhere to the current state; nothing typed is lost or applied twice (in progress) | ❌ |
| 3. AIs: each tab is a real conversation with one AI, reached through a driver process that is a child of the server (one per tab, docs/driver-protocol.md); nothing the server received is lost or shown twice; a prompt reaches the harness at most once; a turn cut by a driver's exit is shown as cut | ❌ |
| 4. Heads-down: timer and queue live on the server; visible queue, stop always gets through, one away-report at the end | ❌ |
| 5. Fleet: see the machines and what runs on each | ❌ |
| The fleet's machine list from the server, as the AIs are: the server sends the machines it knows in its state, and the window stops reading nova-config for the terminal tabs' machine labels, its last outside tool | ❌ |
| 6. Sprint: see and deal cards without leaving the window | ❌ |
| 7. Panels: AIs add UI elements when asked; a panel is data (what to show, which tool's output feeds it, what each button sends) rendered by nexus, never code loaded into it | ❌ |
| One install: nexus-server on macOS, Linux or Windows (WSL2) starts and supervises the configured nova-tools and nova-sprint services as child processes, including Redis, message bus listeners, Sprint scheduling and workers. The host starts nexus-server at boot. Closing a client leaves the services running. iPad and iPhone connect as clients. | ❌ |
| Writer thread: the store's write and flush run off the server loop's thread, and the loop keeps sending state to the windows while a flush runs, holding back only the acknowledgement until it is durable. mosh cannot hold that back: unmodified, it sends as ack_num the newest remote state it has received, on any tick, so the acknowledgement is bound to received state, and modifying mosh is out (AGENTS.md). So the acknowledgement moves up into nexus: the server's state carries each window's durable applied counter (the number the store has flushed), and the window trims its held actions by that counter, not by mosh's ack, and sends them again after a reconnect as it does now; the state shown is the durable state, or marks what is not yet durable. Changes the Link model (the window's held list, ExactlyOnce across a crash), the Applier, the window's trimming, and their trace binding | ❌ |
| Server loop bound to a model: a spec of one pass of nexus-server's loop (the batch applied with the drivers held, its one flush, the drivers released, the state set, the links' ticks at the next pass, and a crash anywhere in it), and the real loop's trace (hold, flush, release, tick, with the fs seam's crashes) checked against it in CI, with reversed witnesses for a release, a state or a tick before the flush | ✅ |
| iPhone depth: a TLA+ layout model and trace check for the momentum list scroll (finger_scroll), as the rules for index arithmetic ask | ❌ |
| iOS lock depth: a trace check binding the iOS lock glue to ClientLock.tla, so the lock the phone shows is the lock the model proves | ❌ |
| Login switch bound to its model: the hub's sign-in-then-switch (SignInClaim naming a session and another account, switchTo, switchSignedIn, dropSwitches, the learned email) driven by a random walk of sign-ins that succeed, fail, are declined or are refused as another account's identity, and logouts, its trace checked by TLC as a behaviour of models/LoginSwitch.tla in CI beside its witnesses | ❌ |
| iOS client (one universal app for iPhone and iPad), a second client of the server: builds, draws and links in the simulator and on an iPad (docs/ios.md); next the phone's own form, voice, and the iPhone's side of the watch's relay (docs/watch.md) | ❌ |
| Install through AI, no installer: a person in their own AI, on their own subscription, points it at this repo; the AI builds nexus from source by README, installs nexus-server as a login item, and sets nexus up with one AI, its own harness and account. No sprint, no fleet. README is the installer and is proven by a cold run on a clean Mac. After the AIs layer. (later) | ❌ |
| Real isolation, for people who need it: everything on the fleet (AIs, fleet workers on API billing, and CI workers) runs inside a sandboxed VM or container, so nothing it runs can reach the machine's own files, logins or services. The token wall today is best effort (docs/secrets.md). Candidates to try, none chosen, by host, in docs/secrets.md "Real isolation: candidates": on Mac hosts Tart (a separate program only: source-available, never taken in), Apple's containerization and container tool, Lima, lume, vfkit, Podman's VM mode; on Linux and WSL2 libkrun through BoxLite or microsandbox, Firecracker with Fireactions for CI runners, Cloud Hypervisor, gVisor, Kata Containers, Podman, Incus; on any host Kubernetes (agent-sandbox, a runner controller) or VMs on GCP or AWS; bubblewrap and nsjail as the process-level baseline. Day-to-day AIs on a Mac stay on the process sandbox (GPU and Xcode natively). (later) | ❌ |
| complete | 5% |

[Source data](roadmap.sexp)

<!-- nova-work:layers:end -->

## Takeover

<!-- nova-work:takeover:start -->

| feature | done |
|---|:---:|
| 1. Store lock: at most one process ever has the store open for writing, enforced by the operating system (an exclusive flock on the store's directory itself, taken before the store is read, held for the life of the process, released by the OS however it ends; the store reached only through that directory's descriptor after); the server starts in the order probe, lock and replay, socket, drivers restored, waiting a bounded time for a writer that holds the lock without answering; only a start refused by a live server exits 0; readers take the lock shared, change nothing and give up past a limit; the lock checked where it lives, and network filesystems refused | ✅ |
| 2. Side-by-side placement with one atomic switch: a new build is placed beside the running one, and one atomic step makes it the one that runs: builds immutable, complete and named by their content hash under builds/, placed through a temporary folder synced and verified before one rename; tested bound to the whole folder's hash; current and previous switched in one rename under the builds directory's lock; a build a running server holds never pruned (docs/builds.md) | ✅ |
| 3. Handover as a durable, resumable state machine under an OS lock: the old server drains and hands over, and a handover cut anywhere resumes from what is on disk (docs/builds.md) | ✅ |
| 4. Supervision, systemd included: the service starts, restarts and rolls back, so that a build that fails later (after it has started) still rolls back (docs/builds.md) | ✅ |
| 5. Old-layout migration and the store's format against rollback: a store from the old ~/.nexus layout moves once, and a store a newer build wrote stays readable by the build rolled back to | ✅ |
| 6. The model bound to the real steps: the takeover's TLA+ model checked against traces of the real placement, handover, supervision and migration in CI | ✅ |
| complete | 100% |

[Source data](roadmap.sexp)

<!-- nova-work:takeover:end -->

## Window features

<!-- nova-work:window-features:start -->

| feature | done |
|---|:---:|
| Let an AI see a display or window the human chooses with Screen Recording permission (later) | ❌ |
| Let an AI click and type in other apps with separate Accessibility permission and a human stop control (later) | ❌ |
| Audit session isolation across the client and server. Exercise two simultaneous sessions, including sessions on one account, and verify that messages, drafts, errors, prompts, actions and delayed replies stay with their originating session through tab switches and reconnects. | ❌ |
| Finish v0.2.5: verify the remaining platform gates, deploy the matching client and policy, merge the release PR, then publish the source archive and checksums | ❌ |
| Compact work groups: keep only the latest three unwrapped lines, cut at the last whole character with no ellipsis; click for the complete wrapped, scrollable text in an 80-percent-width and height popup; Escape dismisses the popup | ❌ |
| AI-only selection: double-click a paragraph to focus the output window; Shift-Up/Down extends or shrinks through natural AI output elements, skipping human and working text | ❌ |
| Enter from a focused AI-output selection, or paste a copied AI selection, inserts a quoted block followed by two newlines; show a distinct quote in the editable prompt and the sent human bubble | ❌ |
| Make the Knight Rider afterglow stronger and decay more slowly; wire scanner_decay_seconds into the actual rendering | ❌ |
| Add a subtle outline to the output panel, matching the prompt outline relative to its different interior color | ✅ |
| Render Markdown tables with native Prax table layout, aligned columns, readable headers and preserved inline formatting and links | ❌ |
| Reduce the header height by six points while preserving notch avoidance, clock alignment and tab hit targets | ❌ |
| Use full-width conversation bubbles and warnings on both phones and tablets | ❌ |
| Prax cursor-rendering hook: match the prompt HDR pulse while preserving shell and editor cursor behavior | ❌ |
| Terminal tabs: a shell per tab, after the AIs; added with Cmd-T, closed with Cmd-W | ✅ |
| Sprint tab: nova-sprint's board, always first and always Cmd-1; never moved, closed or typed into; the page opaque, as nova-sprint serves it | ❌ |
| The plus button uploads a picked file to the server, into the selected AI's incoming/ directory; an image is also posted to the chat (needs layers 2 and 3) (later) | ❌ |
| The upload's storage on the server: each file a blob in the store by its content hash, sent in chunks that resume after a cut, and placed in the AI's incoming/ in one atomic step; waits for the upload design (an earlier draft is in the git history of src/store) (later) | ❌ |
| Hotkeys to choose an AI's model and effort, in place of the model name that stood under the prompt (later) | ❌ |
| HDR and wide gamut: the window asks Prax for its HDR swap chain (half floats, extended sRGB, EDR, gamut-mapped to the display) once Prax's display pass keeps alpha for a see-through window, and once nexus draws colours past sRGB or brighter than SDR white (later) | ❌ |
| Windows: the native window (Prax's Win32 backend) reaches nexus-server inside WSL2 through the window's link run there, nexus-link-relay, over the relay's standard input and output (mosh's transport does not run natively on Windows), with a functional test across that boundary in CI (wsl2-link on captain runs the window's port through WSL interop); its terminal tabs are WSL2 shells in the WSL2 home directory, never cmd or PowerShell | ✅ |
| The window reconnects by itself when its server stops or is replaced, and after a crash, with no message and the state cursor grey meanwhile; it names itself (--client) and resends from its first unacknowledged action (--next) | ✅ |
| Windows depth: hold actions until the relay's Hello is read and the phase is running (wantsWrite, relay_life.cpp:197; the model's Write requires running plus a witness), so a missing or mismatched relay cannot swallow sign-in steps and secrets | ❌ |
| Windows depth: start wsl.exe with PROC_THREAD_ATTRIBUTE_HANDLE_LIST (relay_port_win.cpp:136) so it does not inherit the pipes of other child processes | ❌ |
| Windows depth: one relay at a time holds for wsl.exe, not for the Linux relay; fix the comments and OneRelay to say so, and bind the port's real check in the trace, which relay_walk.cpp:204 re-implements | ❌ |
| Windows depth: close the trace binding gaps (ten of seventeen review plants escaped the trace): constrain ranWell by the relay's run time (RelayLife.tla:171); model the tells as a sequence with a no-repeat invariant; unit tests for the acked_+1 re-cut, acked going down, and ended() after stop(); widen the CI seeds | ❌ |
| Windows depth: a watchdog for a hung relay; keep the NEXUS_WSL_RELAY test hook out of shipping builds; show a notice when history requests are dropped; show a bubble for a message sent when nextAction()==0 (frame_host.cpp:779-783) | ❌ |
| Linux desktop window: waits on Prax's Wayland backend (asked of Patrick); until then the Linux build is the server only | ❌ |
| complete | 14% |

[Source data](roadmap.sexp)

<!-- nova-work:window-features:end -->

## Configuration through your AI

<!-- nova-work:configuration:start -->

| feature | done |
|---|:---:|
| Audit remaining native appearance and input ownership; findings and completion gates in docs/configuration-audit.md | ✅ |
| Route application pointer, keyboard and modal intents through Starlark and validated native operations; preserve native text editing, IME and terminal delivery | ❌ |
| Give every exposed UI setting an identified consumer and behavioral qualification; fix scanner decay and prevent inert settings | ❌ |
| Move remaining presentation defaults from native structs into the shipped profile and generate compatible fallback records from one source | ❌ |
| Make public transcript grouping, compact work previews, details, warning widths and labels configurable while retaining mandatory native privacy redaction | ❌ |
| Move application gesture thresholds and intent precedence into documented policy; test customized profiles through real client input | ❌ |
| Stage and verify a binary and policy from one immutable commit; support compatible live previews and close them with warning-free matching packages | ❌ |
| Fork-owned configuration and Starlark in the client and server. Both native hosts run editable rules with shared modules and preview/apply. The window and conversation applications now implement keyboard routing, panel composition, tab registration, message plans and session defaults (docs/policy.md, docs/plugins.md). Continue moving application behavior behind the native host primitives | ❌ |
| Client config as data: ~/.nexus/client becomes documented, versioned, human-editable JSON in git, with the same verbs and apply report as the server's (docs/accounts.md); a person's AI changes it through the server, which relays the change to the client that asked and never stores, caches or syncs it: each machine's config is its own. Tab layout is client config too: a tab per AI or a tab per instance of an AI (the server sends every session's AI and instance names, and keeps no tabs of its own) | ❌ |
| A client-config template on the server, kept up to date: any change to the template becomes a pending update that each client applies at its next connect; config stays local to each machine, with no live sync. Open question: how a machine's local edits survive an update (proposal: an update replaces only the fields the template change touched) | ❌ |
| Promote one machine's client config to the template, with a pending update for every other machine (later) | ❌ |
| The window's rules that are one person's setup move into the client config: today's fixed zones (sprint, then AIs, then terminals) become the default tab groups, in the default order dashboards, AIs, terminals (tab-groups) | ❌ |
| The server's key-value config and its drivers file become documented JSON in nexus-config, with a version, check, reload and history like accounts.json | ❌ |
| Pushing the config repository to a remote stays opt-in (built: `nexus-server config remote`); later, an in-app GitHub sign-in for the pushes, through the secret dialog, in place of the git credentials of the user nexus-server runs as (later) | ❌ |
| The sprint tab shown only when a board is configured (AGENTS.md, Installing). Two requirements learned in a first try: nothing a person has vanishes on upgrade (a window that had the tab keeps it, and the step that keeps it runs in the real window, never only on a test board, and is marked done only once it has run); and any value discovered for the config, such as the board's address, is validated before it is written (an http:// or https:// address, one line, no control bytes), so nothing can add a line to the config (later) | ❌ |
| Rename an AI as one change in ais.json: its sessions, the window's tab, the tab's place in the saved order and its draft go to the new name. Today a rename is a remove of the old name and an add of the new (docs/accounts.md) (later) | ❌ |
| Question: does the config repository's history ever need pruning, and if so how, without rewriting what was pushed (later) | ❌ |
| complete | 5% |

[Source data](roadmap.sexp)

<!-- nova-work:configuration:end -->

## Tab kinds

<!-- nova-work:tab-kinds:start -->

| feature | done |
|---|:---:|
| Fork-defined registrations, groups and shortcuts support compiled Prax renderers and Starlark panels (docs/plugins.md). Further tab kinds: AI, terminal, sprint board, editor, files, chat, mail, social and more; which tabs a machine shows is set in its client config (and the server's client template), by talking with your AI | ❌ |
| The tab strip is made of tab groups, generalizing today's fixed zones (sprint, then AIs, then terminals): a dashboards group (the sprint board is one dashboard), a AIs group (a tab per AI or per instance, as configured) and a terminals group; later groups for editor, files, chat, mail and social. Which groups exist, their order and their contents are client config, set by talking with your AI; the default order is dashboards, AIs, terminals. Each group's definition carries its behaviour: whether Cmd-W closes its tabs (a group is not closable unless its config says so; terminals are closable by default), how a new tab is made (Cmd-T for terminals), and dragging moves a tab within its group only | ❌ |
| Editor tab, modelled on Sublime Text 3: fast open, a command palette, multiple cursors, Goto Anything; keyboard-first and the same on every OS, with nexus's own look; Prax's highest text layers first. After the AIs layer (later) | ❌ |
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
| Before any nexus binary that links the shim ships: the licence text of every module src/tailscale/NOTICES.md lists goes with it (embedded and shown in the app, or installed beside it), generated from the modules at their pins and checked by CI against NOTICES.md | ✅ |
| 1. Shim API: start with the control server as a parameter (empty for the hosted service, else https only, with an auth key only then, one control server per state directory) and close; a state watch, pollable or by callback (needs login with its URL, starting, running, ready, expired, stopped, failed with a reason), where ready means running and the peer map received; peers by DNS name, who-is with unknown as an answer, TCP listen, accept and dial and UDP listen and dial through the embedded stack with timeouts; a new login that keeps the identity; validated handles, panics stopped at every export, the threading contract stated and tested; the sign-in URL only through the API, never logged; a TLA+ model of the node's lifecycle bound to the code by a trace check; functional tests against tailscale's test control server on loopback | ❌ |
| 2. Client sign-in: the client starts its node in ~/.nexus/client/tailscale, opens the sign-in URL (nts_login_url) in the system browser (in Prax once it has cookies and a system auth sheet), waits for ready, and keeps the state; the first-run OS prompts (macOS Local Network, Windows Firewall) observed on a desktop session; the first sign-in against the hosted service by a person, by hand | ❌ |
| 3. Server enrolment by path A, interactive only (the owner's choice): the server's node hands its sign-in URL to the client, which opens it in the browser already signed in, one Connect click per server; the local server's handoff over the pipe the client spawned it on (macOS, WSL2), and a remote server's first start | ❌ |
| 4. Bootstrap over the tailnet: nexus-server as a per-user service whose tsnet node answers NEXUS CONNECT only to callers who-is (nts_whois_addr) names as the server's own login, an unknown caller refused; the link model extended | ❌ |
| 5. mosh over tsnet: mosh's transport unmodified, through an in-process loopback relay between its 127.0.0.1 socket and the shim's tailnet UDP; a layout model if the relay keeps rings; the added latency measured in milliseconds against the 8.3 ms frame | ❌ |
| 6. Discovery: the client lists the peers whose host name is nexus-server-* (matched and dialed by their DNS name, which a control server may change for a duplicate) and offers them as servers; --server takes a name from that list | ❌ |
| 7. Expiry and recovery shown in the app from the shim's expired and failed states and its key expiry time (an untagged node expires after 180 days); recovery by a new login that keeps the node's identity (nts_login), approved again by the person; ssh retires once layers 4 to 6 are secured | ❌ |
| Later, not built: use your own control server (headscale). An option for people who want to run their own server: they set headscale up themselves, they give its address, and our embedded nodes use it instead of Tailscale's hosted service | ❌ |
| complete | 10% |

[Source data](roadmap.sexp)

<!-- nova-work:tailscale:end -->

## Lessons from pi

Mechanisms taken from pi (earendil-works/pi, MIT), bottom up, each secured before the next starts. Native per-harness drivers stay primary; pi is a source of lessons and possibly one more driver.

<!-- nova-work:lessons-from-pi:start -->

| feature | done |
|---|:---:|
| 1. Driver protocol additions: steer (taken in after the current turn's tool calls, before the next model call) and follow-up (taken in only when the run would otherwise end) as their own kinds beside send; an explicit settled event, since the end of one model run is not done while retries, compaction or queued messages can follow; the full queue sent on each change; each driver says in hello which native features its harness has, so a harness's own features keep working and the server offers only what the driver can do. TLA+ model and fake driver first | ✅ |
| 2. Transcript entries: entries get an id and a parent, so a tab can fork and branch; a compaction entry (a summary and the first entry kept; the entries before it stay in the store and leave only what the model sees); an edit entry that hides an entry from the model without deleting it (a failed overflow attempt, say); a branch summary when a branch is left; usage entries that count cost and never reach the model. A store format bump with its store model, and a durable cursor (entries after an id) for whoever mirrors a session | ❌ |
| 3. The prompt as data, customized by talking with your AI: named sections, changed by the AI on the person's word, checked by the server and appended as a patch (sections set or removed, tools added or removed), never a rewrite of the head, so the prompt cache survives, the prompt now is rebuilt from the transcript, and undo walks the history back. Native drivers apply a patch their own way | ❌ |
| 4. A structured summary for an account switch or a move the harness cannot resume, in place of today's 16 KiB tail of recent turns: goal, constraints and preferences, progress (done, in progress, blocked), key decisions, next steps, critical context; exact paths, names and errors kept; files read and changed carried across summaries; a new summary updates the last one; the conversation given to the summarizer as flat text, never as a conversation to continue, with tool results cut short | ❌ |
| 5. A pi driver for one AI who is not on Claude, through pi's RPC mode (pi --mode rpc, strict LF-delimited JSONL, an id on each command, a prompt's acceptance not its end, done at agent_settled): only after that provider's subscription sign-in is checked against its terms (pi's Claude sign-in presents itself as Claude Code, so Claude AIs never go through pi); and pi's lists of retryable, non-retryable and context-overflow errors taken into every driver, a usage limit reported as usage-limit | ❌ |
| complete | 20% |

[Source data](roadmap.sexp)

<!-- nova-work:lessons-from-pi:end -->

## Apple Watch

Talk to your AIs from the wrist, and they talk back: the watch is a thin client that relays through the iPhone (docs/watch.md says why). Each layer secured before the next starts.

<!-- nova-work:watch:start -->

| feature | done |
|---|:---:|
| 1. The voice turn: hold to talk, the AI named first ("Hey Rowan, ...") or with no name the first AI in the server's list, the reply spoken; every refusal said and shown; locked (not on the wrist since the passcode), it neither listens nor reads a reply | ✅ |
| 2. The relay to the phone: the watch's prompts reach the server once, through restarts of either end, and a phone app not unlocked since it slept refuses the watch, saying so | ✅ |
| 3. The watchOS app: SwiftUI over the C++ core, built for the simulator and a watch in CI, driven end to end in the simulator to an AI on a test server and back | ✅ |
| 4. The iPhone's side of the relay: WatchConnectivity to the nexus iPhone app, its link to nexus-server and its recognizer (waits on the link on iOS) | ❌ |
| 5. On a real watch: signed with the team, installed, and the owner's check and the microphone tried on the wrist | ❌ |
| complete | 60% |

[Source data](roadmap.sexp)

<!-- nova-work:watch:end -->

<!-- nova-work:apple-watch:start -->

| feature | done |
|---|:---:|
| iPhone-side WatchConnectivity relay: the watch's only route to the server, with the phone lock enforced there | ❌ |
| Live, not latched, wrist and lock state: nothing is spoken while locked, including cues and notices (NexusWatchApp.swift:16-20, Client.swift:94-120) | ❌ |
| A core timeout on Listening, so a voice turn that hears nothing ends (voice_turn.cpp:263) | ❌ |
| Two prompts to the same AI lose the first reply: keep both (relay.cpp:483, 414-419) | ❌ |
| Persist across restarts: the relay's events_ and awaits_ over a phone restart (relay.h:225,232); epoch_ and ack_ over a watch restart (relay.cpp:282-292); unapplied_ so lost prompts are told (nexus_watch.cpp:46) | ❌ |
| Mac relay host: a per-launch token and a Content-Type check on every request (watch/host/main.swift:134-171) | ❌ |
| A frame from another watch id must not reset state, nor be accepted while locked (relay.cpp:335-348) | ❌ |
| Extend Relay.tla and relay_walk to the lock, events and replies, audio, a watch switch and an unknown AI; three review plants escaped (relay.cpp:452, 360, 272) | ❌ |
| Speak recorder failures aloud, and validate the audio format; the model tests are too slow under load | ❌ |
| A fleet Mac with Xcode (mini-m5) for the nightly speech test | ❌ |
| Run on a real paired watch | ❌ |
| complete | 0% |

[Source data](roadmap.sexp)

<!-- nova-work:apple-watch:end -->

## Client lock

<!-- nova-work:client-lock:start -->

| feature | done |
|---|:---:|
| Bind the lock trace check to the view's real lock screen and input gate: the walk drives WindowLock and FrameHost::event, not ClientLock alone. | ❌ |
| complete | 0% |

[Source data](roadmap.sexp)

<!-- nova-work:client-lock:end -->
