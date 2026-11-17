; docs/roadmap.sexp: the data the tables in ROADMAP.md are generated from (nova-roadmap render).
; The layers of nexus, bottom up. Each is secured before the next starts.
(:schema 1
 :root "goal"
 :page (:roadmaps ("layers" "takeover" "window-features" "configuration" "tab-kinds" "tailscale"))
 :nodes
 ((:id "goal" :type :work-set :children ("layers" "takeover" "window-features" "configuration" "tab-kinds" "tailscale"))
  (:id "takeover" :type :roadmap :title "Takeover: a new build replaces the running server, rebuilt bottom up"
   :rows (
     ("takeover-store-lock" "1. Store lock: at most one process ever has the store open for writing, enforced by the operating system (an exclusive flock on the store's directory itself, taken before the store is read, held for the life of the process, released by the OS however it ends; the store reached only through that directory's descriptor after); the server starts in the order probe, lock and replay, socket, drivers restored, waiting a bounded time for a writer that holds the lock without answering; only a start refused by a live server exits 0; readers take the lock shared, change nothing and give up past a limit; the lock checked where it lives, and network filesystems refused (in progress)")
     ("takeover-placement" "2. Side-by-side placement with one atomic switch: a new build is placed beside the running one, and one atomic step makes it the one that runs: builds immutable, complete and named by their content hash under builds/, placed through a temporary folder synced and verified before one rename; tested bound to the whole folder's hash; current and previous switched in one rename under the builds directory's lock; a build a running server holds never pruned (docs/builds.md) (in progress)")
     ("takeover-handover" "3. Handover as a durable, resumable state machine under an OS lock: the old server drains and hands over, and a handover cut anywhere resumes from what is on disk")
     ("takeover-supervision" "4. Supervision, systemd included: the service starts, restarts and rolls back, so that a build that fails later (after it has started) still rolls back")
     ("takeover-migration" "5. Old-layout migration and the store's format against rollback: a store from the old ~/.nexus layout moves once, and a store a newer build wrote stays readable by the build rolled back to")
     ("takeover-binding" "6. The model bound to the real steps: the takeover's TLA+ model checked against traces of the real placement, handover, supervision and migration in CI")
     )
   :columns (("done" "done"))
   :cells (
     ("takeover-store-lock" "done" "t-takeover-store-lock")
     ("takeover-placement" "done" "t-takeover-placement")
     ("takeover-handover" "done" "t-takeover-handover")
     ("takeover-supervision" "done" "t-takeover-supervision")
     ("takeover-migration" "done" "t-takeover-migration")
     ("takeover-binding" "done" "t-takeover-binding")
     ))
  (:id "t-takeover-store-lock" :type :task :title "Store lock: one writer by the OS's lock, taken before the store is read; start-up order lock, replay, socket, drivers" :state :doing
   :evidence ("models/store/StoreLock.tla: ctest -L model tlc_store_lock_*" "tests/store/store_lock_walk.cpp: ctest -L model tlc_store_lock_trace_*" "tests/store/store_test.cpp: ctest -L unit store_unit_lock store_unit_start_order" "tests/store/store_functional_test.cpp: ctest -L functional store_functional" "tests/link/server_lock.cpp: ctest -L functional server_lock_functional"))
  (:id "t-takeover-placement" :type :task :title "Side-by-side placement with one atomic switch" :state :doing
   :evidence ("models/placement/Placement.tla: ctest -L model tlc_placement_spec tlc_placement_seq tlc_placement_witness_*" "tests/server/builds_walk.cpp: ctest -L model tlc_placement_trace_*" "tests/server/builds_test.cpp: ctest -L unit builds_unit" "tests/server/builds_functional.cpp: ctest -L functional builds_functional" "docs/builds.md"))
  (:id "t-takeover-handover" :type :task :title "Handover as a durable, resumable state machine under an OS lock" :state :todo)
  (:id "t-takeover-supervision" :type :task :title "Supervision, systemd included, so a build that fails later still rolls back" :state :todo)
  (:id "t-takeover-migration" :type :task :title "Old-layout migration, and the store's format against rollback" :state :todo)
  (:id "t-takeover-binding" :type :task :title "The takeover's model bound to the real steps by trace checks in CI" :state :todo)
  (:id "layers" :type :roadmap :title "Layers"
   :rows (
     ("window" "1. Window: tabs, transcript and prompt draw and take input (in progress)")
     ("server-sync" "2. Server and sync: a server holds tabs, transcripts and drafts and keeps running without a client; the client reconnects from anywhere to the current state; nothing typed is lost or applied twice (in progress)")
     ("friends" "3. Friends: each tab is a real conversation with one friend, reached through a driver process that is a child of the server (one per tab, docs/driver-protocol.md); nothing the server received is lost or shown twice; a prompt reaches the harness at most once; a turn cut by a driver's exit is shown as cut")
     ("heads-down" "4. Heads-down: timer and queue live on the server; visible queue, stop always gets through, one away-report at the end")
     ("fleet" "5. Fleet: see the machines and what runs on each")
     ("machines-from-server" "The fleet's machine list from the server, as the friends are: the server sends the machines it knows in its state, and the window stops reading nova-config for the terminal tabs' machine labels, its last outside tool")
     ("sprint" "6. Sprint: see and deal cards without leaving the window")
     ("panels" "7. Panels: friends add UI elements when asked; a panel is data (what to show, which tool's output feeds it, what each button sends) rendered by nexus, never code loaded into it")
     ("one-app" "Later: one app, nexus-server, starts at boot on the always-on machine and runs every tool and every friend as its own child process, so permissions are granted once to that app and nothing else runs as a separate app or daemon")
     ("writer-thread" "Writer thread: the store's write and flush run off the server loop's thread, and the loop keeps sending state to the windows while a flush runs, holding back only the acknowledgement until it is durable. mosh cannot hold that back: unmodified, it sends as ack_num the newest remote state it has received, on any tick, so the acknowledgement is bound to received state, and modifying mosh is out (AGENTS.md). So the acknowledgement moves up into nexus: the server's state carries each window's durable applied counter (the number the store has flushed), and the window trims its held actions by that counter, not by mosh's ack, and sends them again after a reconnect as it does now; the state shown is the durable state, or marks what is not yet durable. Changes the Link model (the window's held list, ExactlyOnce across a crash), the Applier, the window's trimming, and their trace binding")
     ("loop-trace" "Server loop bound to a model: a spec of one pass of nexus-server's loop (the batch applied with the drivers held, its one flush, the drivers released, the state set, the links' ticks at the next pass, and a crash anywhere in it), and the real loop's trace (hold, flush, release, tick, with the fs seam's crashes) checked against it in CI, with reversed witnesses for a release, a state or a tick before the flush. Today the order is held by tests/server/pass_test.cpp and link_e2e's flush run, not by a trace")
     ("ios-client" "iOS client (one universal app for iPhone and iPad), a second client of the server; after layer 2, waiting on Prax's iOS app backend" :later t)
     ("install-through-ai" "Install through AI, no installer: a person in their own AI, on their own subscription, points it at this repo; the AI builds nexus from source by README, installs nexus-server as a login item, and sets nexus up with one friend, its own harness and account. No sprint, no fleet. README is the installer and is proven by a cold run on a clean Mac. After the friends layer." :later t)
     ("real-isolation" "Real isolation, for people who need it: everything on the fleet (friends, fleet workers on API billing, and CI workers) runs inside a sandboxed VM or container, so nothing it runs can reach the machine's own files, logins or services. The token wall today is best effort (docs/secrets.md). Candidates to try, none chosen, by host, in docs/secrets.md \"Real isolation: candidates\": on Mac hosts Tart (a separate program only: source-available, never taken in), Apple's containerization and container tool, Lima, lume, vfkit, Podman's VM mode; on Linux and WSL2 libkrun through BoxLite or microsandbox, Firecracker with Fireactions for CI runners, Cloud Hypervisor, gVisor, Kata Containers, Podman, Incus; on any host Kubernetes (agent-sandbox, a runner controller) or VMs on GCP or AWS; bubblewrap and nsjail as the process-level baseline. Day-to-day friends on a Mac stay on the process sandbox (GPU and Xcode natively)." :later t)
     )
   :columns (("done" "done"))
   :cells (
     ("window" "done" "t-window")
     ("server-sync" "done" "t-server-sync")
     ("friends" "done" "t-friends")
     ("heads-down" "done" "t-heads-down")
     ("fleet" "done" "t-fleet")
     ("machines-from-server" "done" "t-machines-from-server")
     ("sprint" "done" "t-sprint")
     ("panels" "done" "t-panels")
     ("one-app" "done" "t-one-app")
     ("writer-thread" "done" "t-writer-thread")
     ("loop-trace" "done" "t-loop-trace")
     ("ios-client" "done" "t-ios-client")
     ("install-through-ai" "done" "t-install-through-ai")
     ("real-isolation" "done" "t-real-isolation")
     ))
  (:id "window-features" :type :roadmap :title "Window features"
   :rows (
     ("terminal-tabs" "Terminal tabs: a shell per tab, after the friends; added with Cmd-T, closed with Cmd-W")
     ("sprint-tab" "Sprint tab: nova-sprint's board, always first and always Cmd-1; never moved, closed or typed into; the page opaque, as nova-sprint serves it")
     ("upload" "The plus button uploads a picked file to the server, into the selected AI's incoming/ directory; an image is also posted to the chat (needs layers 2 and 3)" :later t)
     ("upload-store" "The upload's storage on the server: each file a blob in the store by its content hash, sent in chunks that resume after a cut, and placed in the AI's incoming/ in one atomic step; waits for the upload design (an earlier draft is in the git history of src/store)" :later t)
     ("model-effort-keys" "Hotkeys to choose a friend's model and effort, in place of the model name that stood under the prompt" :later t)
     ("hdr" "HDR and wide gamut: the window asks Prax for its HDR swap chain (half floats, extended sRGB, EDR, gamut-mapped to the display) once Prax's display pass keeps alpha for a see-through window, and once nexus draws colours past sRGB or brighter than SDR white" :later t)
     ("windows-wsl2" "Windows: the native overlay window reaches nexus-server inside WSL2 over a local socket, with a functional test across that boundary in CI (wsl2-functional on captain drives the native window through WSL interop); its terminal tabs are WSL2 shells in the WSL2 home directory, never cmd or PowerShell. Waits on Prax: the window needs Prax's Win32 lib/app to compile, and nexus-server in WSL2 needs Prax's lib/process to link on Linux" :later t)
     )
   :columns (("done" "done"))
   :cells (
     ("terminal-tabs" "done" "t-terminal-tabs")
     ("sprint-tab" "done" "t-sprint-tab")
     ("upload" "done" "t-upload")
     ("upload-store" "done" "t-upload-store")
     ("model-effort-keys" "done" "t-model-effort-keys")
     ("hdr" "done" "t-hdr")
     ("windows-wsl2" "done" "t-windows-wsl2")
     ))
  (:id "configuration" :type :roadmap :title "Configuration through your AI"
   :rows (
     ("client-config" "Client config as data: ~/.nexus/client becomes documented, versioned, human-editable JSON in git, with the same verbs and apply report as the server's (docs/accounts.md); a person's AI changes it through the server, which relays the change to the client that asked and never stores, caches or syncs it: each machine's config is its own. Tab layout is client config too: a tab per friend or a tab per instance of a friend (the server sends every session's friend and instance names, and keeps no tabs of its own)")
     ("client-template" "A client-config template on the server, kept up to date: any change to the template becomes a pending update that each client applies at its next connect; config stays local to each machine, with no live sync. Open question: how a machine's local edits survive an update (proposal: an update replaces only the fields the template change touched)")
     ("client-promote" "Promote one machine's client config to the template, with a pending update for every other machine" :later t)
     ("ui-rules-to-config" "The window's rules that are one person's setup move into the client config: today's fixed zones (sprint, then friends, then terminals) become the default tab groups, in the default order dashboards, friends, terminals (tab-groups)")
     ("server-config-json" "The server's key-value config and its drivers file become documented JSON in nexus-config, with a version, check, reload and history like accounts.json")
     ("config-remote-signin" "Pushing the config repository to a remote stays opt-in (built: `nexus-server config remote`); later, an in-app GitHub sign-in for the pushes, through the secret dialog, in place of the git credentials of the user nexus-server runs as" :later t)
     ("sprint-tab-from-config" "The sprint tab shown only when a board is configured (AGENTS.md, Installing). Two requirements learned in a first try: nothing a person has vanishes on upgrade (a window that had the tab keeps it, and the step that keeps it runs in the real window, never only on a test board, and is marked done only once it has run); and any value discovered for the config, such as the board's address, is validated before it is written (an http:// or https:// address, one line, no control bytes), so nothing can add a line to the config" :later t)
     ("friend-rename" "Rename a friend as one change in friends.json: its sessions, the window's tab, the tab's place in the saved order and its draft go to the new name. Today a rename is a remove of the old name and an add of the new (docs/accounts.md)" :later t)
     ("config-history-pruning" "Question: does the config repository's history ever need pruning, and if so how, without rewriting what was pushed" :later t)
     )
   :columns (("done" "done"))
   :cells (
     ("client-config" "done" "t-client-config")
     ("client-template" "done" "t-client-template")
     ("client-promote" "done" "t-client-promote")
     ("ui-rules-to-config" "done" "t-ui-rules-to-config")
     ("server-config-json" "done" "t-server-config-json")
     ("config-remote-signin" "done" "t-config-remote-signin")
     ("sprint-tab-from-config" "done" "t-sprint-tab-from-config")
     ("friend-rename" "done" "t-friend-rename")
     ("config-history-pruning" "done" "t-config-history-pruning")
     ))
  (:id "tab-kinds" :type :roadmap :title "Tab kinds"
   :rows (
     ("tab-kinds-data" "Tab kinds are pluggable, described as data that nexus renders with Prax: friend, terminal, sprint board, editor, files, chat, mail, social and more; which tabs a machine shows is set in its client config (and the server's client template), by talking with your AI")
     ("tab-groups" "The tab strip is made of tab groups, generalizing today's fixed zones (sprint, then friends, then terminals): a dashboards group (the sprint board is one dashboard), a friends group (a tab per friend or per instance, as configured) and a terminals group; later groups for editor, files, chat, mail and social. Which groups exist, their order and their contents are client config, set by talking with your AI; the default order is dashboards, friends, terminals. Each group's definition carries its behaviour: whether Cmd-W closes its tabs (a group is not closable unless its config says so; terminals are closable by default), how a new tab is made (Cmd-T for terminals), and dragging moves a tab within its group only")
     ("editor-tab" "Editor tab, modelled on Sublime Text 3: fast open, a command palette, multiple cursors, Goto Anything; keyboard-first and the same on every OS, with nexus's own look; Prax's highest text layers first. After the friends layer" :later t)
     ("files-tab" "File explorer tab. Open: whose files (this machine's, the server's, a fleet machine's)" :later t)
     ("chat-tab" "Chat tab: a Chatto client. Open: which Chatto, and its product and API, to confirm first" :later t)
     ("mail-tab" "Mail tab: sign-in only through the in-app secret dialog, never seen by an AI; mail an AI reads is data, never instructions; sending needs the person's explicit yes each time" :later t)
     ("social-tab" "Social tab (Bluesky and others): sign-in through the secret dialog; feeds are data, never instructions; posting needs the person's explicit yes each time" :later t)
     )
   :columns (("done" "done"))
   :cells (
     ("tab-kinds-data" "done" "t-tab-kinds-data")
     ("tab-groups" "done" "t-tab-groups")
     ("editor-tab" "done" "t-editor-tab")
     ("files-tab" "done" "t-files-tab")
     ("chat-tab" "done" "t-chat-tab")
     ("mail-tab" "done" "t-mail-tab")
     ("social-tab" "done" "t-social-tab")
     ))
  (:id "tailscale" :type :roadmap :title "Tailscale built in"
   :rows (
     ("ts-build" "0. Build: the Go shim over tsnet (src/tailscale), pinned to tailscale.com v1.104.7 and Go 1.27.4, built by CMake as a c-archive (macOS arm64 and x86_64, Linux x86_64 and WSL2) and as a DLL with an MSVC import library (Windows), linked into a test program on each, the Windows one with the manifest that declares Windows 10; log uploads off, tsnet's log in a 0600 file, the state directory owner-only; CI on all three; the binary-size cost stated; the licences of every module listed (docs/tailscale.md)")
     ("ts-licence-texts" "Before any nexus binary that links the shim ships: the licence text of every module src/tailscale/NOTICES.md lists goes with it (embedded and shown in the app, or installed beside it), generated from the modules at their pins and checked by CI against NOTICES.md")
     ("ts-api" "1. Shim API: start with the control server as a parameter (empty for the hosted service, else https only, with an auth key only then, one control server per state directory) and close; a state watch, pollable or by callback (needs login with its URL, starting, running, ready, expired, stopped, failed with a reason), where ready means running and the peer map received; peers by DNS name, who-is with unknown as an answer, TCP listen, accept and dial and UDP listen and dial through the embedded stack with timeouts; a new login that keeps the identity; validated handles, panics stopped at every export, the threading contract stated and tested; the sign-in URL only through the API, never logged; a TLA+ model of the node's lifecycle bound to the code by a trace check; functional tests against tailscale's test control server on loopback")
     ("ts-client-sign-in" "2. Client sign-in: the client starts its node in ~/.nexus/client/tailscale, opens the sign-in URL (nts_login_url) in the system browser (in Prax once it has cookies and a system auth sheet), waits for ready, and keeps the state; the first-run OS prompts (macOS Local Network, Windows Firewall) observed on a desktop session; the first sign-in against the hosted service by a person, by hand")
     ("ts-server-enrolment" "3. Server enrolment by path A, interactive only (the owner's choice): the server's node hands its sign-in URL to the client, which opens it in the browser already signed in, one Connect click per server; the local server's handoff over the pipe the client spawned it on (macOS, WSL2), and a remote server's first start")
     ("ts-bootstrap" "4. Bootstrap over the tailnet: nexus-server as a per-user service whose tsnet node answers NEXUS CONNECT only to callers who-is (nts_whois_addr) names as the server's own login, an unknown caller refused; the link model extended")
     ("ts-mosh" "5. mosh over tsnet: mosh's transport unmodified, through an in-process loopback relay between its 127.0.0.1 socket and the shim's tailnet UDP; a layout model if the relay keeps rings; the added latency measured in milliseconds against the 8.3 ms frame")
     ("ts-discovery" "6. Discovery: the client lists the peers whose host name is nexus-server-* (matched and dialed by their DNS name, which a control server may change for a duplicate) and offers them as servers; --server takes a name from that list")
     ("ts-expiry" "7. Expiry and recovery shown in the app from the shim's expired and failed states and its key expiry time (an untagged node expires after 180 days); recovery by a new login that keeps the node's identity (nts_login), approved again by the person; ssh retires once layers 4 to 6 are secured")
     ("ts-own-control" "Later, not built: use your own control server (headscale). An option for people who want to run their own server: they set headscale up themselves, they give its address, and our embedded nodes use it instead of Tailscale's hosted service")
     )
   :columns (("done" "done"))
   :cells (
     ("ts-build" "done" "t-ts-build")
     ("ts-licence-texts" "done" "t-ts-licence-texts")
     ("ts-api" "done" "t-ts-api")
     ("ts-client-sign-in" "done" "t-ts-client-sign-in")
     ("ts-server-enrolment" "done" "t-ts-server-enrolment")
     ("ts-bootstrap" "done" "t-ts-bootstrap")
     ("ts-mosh" "done" "t-ts-mosh")
     ("ts-discovery" "done" "t-ts-discovery")
     ("ts-expiry" "done" "t-ts-expiry")
     ("ts-own-control" "done" "t-ts-own-control")
     ))
  (:id "t-ts-build" :type :task :title "Tailscale 0. Build: the pinned shim on macOS, Linux and Windows, in CI, sizes and licences stated" :state :doing
   :evidence ("src/tailscale/shim, src/tailscale/nts.h, cmake/tailscale.cmake, cmake/windows10.manifest" "tests/tailscale/tailscale_test.cpp: ctest -L tailscale (tailscale_unit_version, tailscale_functional_state_dir, tailscale_functional_refusals, tailscale_functional_refuses_inherited_key, tailscale_functional_lock, tailscale_functional_lock_two_process, tailscale_functional_panics, tailscale_functional_log_cap, tailscale_functional_quiet)" ".github/workflows/tailscale.yml: tailscale-macos, tailscale-linux, tailscale-windows and the commit statuses tailscale-<os>" "src/tailscale/NOTICES.md: cmake --build <dir> --target nexus_tailscale_notices_check" "docs/tailscale.md"))
  (:id "t-ts-own-control" :type :task :title "Tailscale, later: use your own control server (headscale), set up by the person, by its address" :state :todo)
  (:id "t-ts-licence-texts" :type :task :title "Tailscale: the licence texts of the shim's modules carried by every binary that links it, before one ships" :state :todo)
  (:id "t-ts-api" :type :task :title "Tailscale 1. Shim API: lifecycle, state watch, peers, WhoIs, TCP and UDP; a TLA+ model with a trace check" :state :doing
   :evidence ("src/tailscale/nts.h, src/tailscale/shim, src/tailscale/lifecycle" "models/Tailscale.tla, models/MCTailscale.cfg, models/witnesses/WTs*.cfg, models/TailscaleTrace.tla: tlc_tailscale_*" "tests/tailscale/tailscale_test.cpp: ctest -L tailscale, the unit groups and the functional groups against tailscale's test control server on loopback (src/tailscale/shim/hooks_control.go)" "docs/tailscale.md, Layer 1"))
  (:id "t-ts-client-sign-in" :type :task :title "Tailscale 2. Client sign-in through the system browser; the first-run OS prompts observed on a desktop session" :state :todo)
  (:id "t-ts-server-enrolment" :type :task :title "Tailscale 3. Server enrolment by path A: the server's URL opened by the client, one Connect click per server" :state :todo)
  (:id "t-ts-bootstrap" :type :task :title "Tailscale 4. Bootstrap over the tailnet: a per-user service and a WhoIs-gated CONNECT" :state :todo)
  (:id "t-ts-mosh" :type :task :title "Tailscale 5. mosh over tsnet through a loopback relay, its latency measured in ms" :state :todo)
  (:id "t-ts-discovery" :type :task :title "Tailscale 6. Discovery of nexus servers among the tailnet's peers" :state :todo)
  (:id "t-ts-expiry" :type :task :title "Tailscale 7. Expiry and recovery in the app; ssh retired once 4 to 6 are secured" :state :todo)
  (:id "t-client-config" :type :task :title "Client config as documented, versioned JSON in git, relayed by the server, never stored by it" :state :todo)
  (:id "t-client-template" :type :task :title "Client-config template on the server: pending updates applied at each client's next connect" :state :todo)
  (:id "t-client-promote" :type :task :title "Promote one machine's client config to the template" :state :todo)
  (:id "t-ui-rules-to-config" :type :task :title "Sprint tab at Command-1, friend tabs and terminals move into the client config" :state :todo)
  (:id "t-server-config-json" :type :task :title "Server key-value config and drivers file as documented JSON in nexus-config" :state :todo)
  (:id "t-config-remote-signin" :type :task :title "In-app GitHub sign-in for config pushes" :state :todo)
  (:id "t-config-history-pruning" :type :task :title "Config history pruning: whether and how" :state :todo)
  (:id "t-sprint-tab-from-config" :type :task :title "The sprint tab only when a board is configured, with nothing lost on upgrade and every discovered value validated before it is written" :state :todo)
  (:id "t-friend-rename" :type :task :title "Rename a friend as one change: sessions, tab, saved order and draft" :state :todo)
  (:id "t-tab-kinds-data" :type :task :title "Tab kinds as data, chosen per machine in the client config" :state :todo)
  (:id "t-tab-groups" :type :task :title "Tab groups (dashboards, friends, terminals; later more) as client config, each with its own close, new-tab and drag rules" :state :todo)
  (:id "t-editor-tab":type :task :title "Editor tab after Sublime Text 3" :state :todo)
  (:id "t-files-tab" :type :task :title "File explorer tab" :state :todo)
  (:id "t-chat-tab" :type :task :title "Chat tab: a Chatto client" :state :todo)
  (:id "t-mail-tab" :type :task :title "Mail tab" :state :todo)
  (:id "t-social-tab" :type :task :title "Social tab" :state :todo)
  (:id "t-windows-wsl2" :type :task :title "Windows: native overlay window to nexus-server in WSL2 over a local socket, tested across the boundary in CI; terminal tabs are WSL2 shells" :state :todo
   :note "Terminal tabs done on 2026-11-15: src/terminal_launch.h starts the default WSL2 distribution's login bash in the WSL2 home (terminal_launch_unit everywhere; wsl2_terminal, the launch line on a real Windows pseudoconsole, in CI's wsl2-terminal on captain); a missing WSL2 is one line in the tab. nexus's own terminals (src/terminals.cpp) still build only with the window, so on Windows they wait on (1). Missing on 2026-11-14: (1) the native window: NEXUS_WINDOW is off on Windows because Prax's lib/app Win32 backend does not compile at the pin; (2) nexus-server inside WSL2: NEXUS_WINDOW_LOGIC and NEXUS_LINK are off on Linux because Prax's lib/process POSIX backend does not link there (environ declared inside its namespace); (3) the local socket itself: the window reaches nexus-server only through the mosh link with its ssh bootstrap, whose network layer is POSIX and is not built natively on Windows, and nexus-server's own socket is a Unix socket inside WSL2's filesystem, which WSL2 does not share with Windows processes; no transport for the native window to WSL2 exists yet; (4) terminal tabs: src/terminals.cpp left the program empty, so on Windows Prax started COMSPEC or cmd.exe (lib/studio/terminal terminal.cpp) in the Windows home directory (fixed 2026-11-15, above).")
  (:id "t-terminal-tabs" :type :task :title "Terminal tabs: a shell per tab after the friends" :state :done
   :evidence ("tests/window/test_terminal.cpp" "tests/model: tlc_witness_add_before_friend, tlc_trace_faulty_close_rejected"))
  (:id "t-sprint-tab" :type :task :title "Sprint tab: nova-sprint's board, first and Cmd-1, pinned" :state :doing)
  (:id "t-model-effort-keys" :type :task :title "Hotkeys to choose a friend's model and effort" :state :todo)
  (:id "t-hdr" :type :task :title "HDR and wide gamut: Prax's HDR swap chain, once its display pass keeps a see-through window's alpha" :state :todo)
  (:id "t-upload" :type :task :title "Upload: the plus button sends a file to the selected AI's incoming/, images to the chat too" :state :todo)
  (:id "t-upload-store" :type :task :title "Upload storage: blobs by content hash in the store, chunks that resume, atomic placement in incoming/" :state :todo)  (:id "t-window" :type :task :title "Window: tabs, transcript and prompt draw and take input" :state :doing)
  (:id "t-server-sync" :type :task :title "Server and sync: server holds the state; mosh-style sync over the tailnet; nothing typed lost or applied twice" :state :doing)
  (:id "t-wire-version" :type :task :title "Wire version: the window names the wire it speaks in its new request, and the server refuses another with which side to update (today an older window against a newer server fails its diffs)" :state :todo)
  (:id "t-writer-thread" :type :task :title "Writer thread: store write and flush off the loop thread; an app-level durable counter in the server's state, the window trimming by it; Link model, Applier and trace binding" :state :todo)
  (:id "t-loop-trace" :type :task :title "Server loop bound to a model: a spec of one pass (hold, flush, release, tick, crash) and the real loop's traces checked against it in CI" :state :todo)
  (:id "t-session-close" :type :task :title "Closing a session: sessions are never removed, so the limit of 16 counts every session ever made; a closed session frees its place, its id still never reused" :state :todo)
  (:id "t-friends" :type :work-set :children ("t-friends-protocol" "t-friends-supervisor" "t-friends-wire"
                                               "t-friends-sessions" "t-friends-claude" "t-friends-claude-model"
                                               "t-friends-accounts"
                                               "t-friends-window"))
  (:id "t-friends-protocol" :type :task :title "1. Driver protocol and a fake driver: the line protocol, its reader, and a scripted driver for the server's tests" :state :done
   :evidence ("tests/driver/protocol_test.cpp: ctest -L unit driver_unit_*" "tests/driver/fake_driver_test.cpp: ctest -L functional driver_fake_*" "docs/driver-protocol.md"))
  (:id "t-friends-supervisor" :type :task :title "2. Driver supervisor in the server: starts, restarts and resumes each tab's driver; modelled in TLA+ with the protocol's sequence rules" :state :done
   :evidence ("models/Supervisor.tla: ctest -L model tlc_supervisor*" "tests/server/supervisor_test.cpp: ctest -L unit supervisor_unit_*" "tests/server/drivers_functional.cpp: ctest -L functional friends_functional_*" "tests/link/link_e2e.cpp: ctest -L functional link_e2e_loopback"))
  (:id "t-friends-wire" :type :task :title "3. Wire and transcript: turns, text blocks, tools and outcomes carried to the window" :state :done
   :evidence ("models/Link.tla, models/Window.tla, models/Supervisor.tla: ctest -L model tlc_link_* tlc_witness_take_newest tlc_supervisor_*" "tests/sync/link_walk.cpp: ctest -L model tlc_link_trace_*" "tests/sync/sync_test.cpp: ctest -L unit sync_unit" "tests/store/store_test.cpp, tests/store/durable_test.cpp: ctest -L unit store_unit_dequeue durable_unit_take" "tests/server/supervisor_test.cpp: ctest -L unit supervisor_unit_queue supervisor_unit_stop supervisor_unit_stop_hammered" "tests/server/drivers_functional.cpp: ctest -L functional friends_functional_queue friends_functional_queue_unconfigured friends_functional_restart_unconfigured friends_functional_stop friends_functional_stop_hammered"))
  (:id "t-friends-sessions" :type :task :title "3b. Sessions: the server mints session ids (never reused) and keys store, wire and drivers by them; create, ensure and set account, model or effort are decided on the server; an account change restarts the session's driver with its id and transcript; friend-tab stores open as one session per friend" :state :done
   :evidence ("models/Link.tla: ctest -L model tlc_link_sessions tlc_link_ensure tlc_link_witness_id_reuse tlc_link_witness_ensure_again tlc_link_witness_show_newest" "tests/sync/link_walk.cpp: ctest -L model tlc_link_trace_* tlc_link_trace_faulty_ensure_rejected" "models/Supervisor.tla: ctest -L model tlc_supervisor_switch_temporal_1 tlc_supervisor_switch_temporal_2 tlc_supervisor_switch_rest tlc_supervisor_witness_setting_* tlc_supervisor_witness_account_* tlc_supervisor_trace_faulty_setting_rejected tlc_supervisor_trace_faulty_account_rejected" "tests/sync/sync_test.cpp: ctest -L unit sync_unit" "tests/store/store_test.cpp: ctest -L unit store_unit_ids store_unit_migrate" "tests/store/durable_test.cpp: ctest -L unit durable_unit_restart durable_unit_migrate" "tests/server/drivers_functional.cpp: ctest -L functional friends_functional_account" "tests/link/link_e2e.cpp: ctest -L functional link_e2e_*"))
  (:id "t-friends-claude" :type :task :title "4. Claude driver on the Agent SDK" :state :done
   :evidence ("drivers/claude/test/unit: npm test (scripted query: turns, stop, resume, session-not-found, settings, subscription refusals, protocol refusals)" "tests/driver/conformance_test.cpp: ctest -L claude_driver driver_conformance_claude_* (and -L functional driver_conformance_fake_*)" "tests/driver/protocol_test.cpp: ctest -L unit driver_unit_* (settings)" "drivers/claude/test/live/live.ts: npm run test:live, opt-in, on the subscription" "docs/driver-protocol.md: settings"))
  (:id "t-friends-claude-model" :type :task :title "Claude driver's turn state machine (credential check, push, stop and interrupt, settings, resume) in TLA+, bound by trace checks of the driver's unit walks" :state :done
   :evidence ("models/ClaudeDriver.tla: ctest -L model tlc_claude_driver tlc_claude_driver_witness_*" "drivers/claude/test/walk.ts, drivers/claude/test/unit/driver.test.ts: ctest -L model tlc_claude_driver_trace_*"))
  (:id "t-friends-accounts" :type :work-set :children ("t-accounts-server" "t-accounts-secret-dialog"
                                                       "t-accounts-sign-in" "t-accounts-drivers"
                                                       "t-accounts-window" "t-accounts-remote-instance"
                                                       "t-accounts-friend-limit"))
  (:id "t-accounts-server" :type :task :title "5a. Accounts and friends on the server: providers have accounts; friends link to any number of accounts of any providers, with a default; a friend has instances (a machine and a working directory each), and a session belongs to one of them; a session runs on one of its friend's accounts, set by the person or rotated by the server at a usage limit; accounts.json and friends.json in a git repository of their own, edited through the person's AI with `nexus-server config check`, `reload`, `history`, `restore` and `remote`" :state :done
   :evidence ("models/Accounts.tla: ctest -L model tlc_accounts tlc_accounts_witness_*" "tests/accounts/accounts_walk.cpp: ctest -L model tlc_accounts_trace_*" "tests/accounts/accounts_test.cpp: ctest -L unit accounts_unit_*" "tests/accounts/config_repo_test.cpp: ctest -L functional accounts_functional_config_repo"))
  (:id "t-accounts-secret-dialog" :type :task :title "5b. Secret dialog: the window's own dialog for a credential, sent to the server and written 0600 under ~/.nexus/server/accounts/<name>/; never shown, logged, or seen by an AI" :state :done
   :evidence ("models/Secrets.tla: ctest -L model tlc_secrets_spec tlc_secrets_spec_one tlc_secrets_spec_old tlc_secrets_spec_bad tlc_secrets_witness_* (17 witnesses, ReceivedNeverDropped among the properties: a pasted value is never lost while the server runs); -L model_nightly tlc_secrets_spec_two tlc_secrets_spec_wide tlc_secrets_spec_mixed" "tests/sync/secrets_walk.cpp, models/SecretsTrace.tla: ctest -L model tlc_secrets_trace_seed_* tlc_secrets_trace_faulty_*_rejected" "tests/server/secrets_test.cpp: ctest -L unit secrets_unit_* (secrets_unit_leak: a fake value searched for in every output), -L functional secrets_functional_files_disk secrets_functional_login_files" "src/server/login_life.h: ctest -L unit secrets_unit_login_life (a claude login's year, its notices, when a paste is asked for)" "tests/window/test_frames.cpp: ctest -L functional window_frame_budget (secret_dialog)" "tests/driver/protocol_test.cpp: ctest -L unit driver_unit_secrets" "tests/server/supervisor_test.cpp, supervisor_walk.cpp: ctest -L unit supervisor_unit_signin; -L model tlc_supervisor_trace_* (SignIn)" "docs/secrets.md, docs/formats.md (state format 3), docs/driver-protocol.md (server, ask-secret, secret)"))
  (:id "claude-ask-secret-tool" :type :task :title "Claude driver: the nexus_ask_secret(account, what, why) tool for its AI, writing ask-secret after a server line naming secrets and returning stored or refused: <reason> from the secret line (docs/secrets.md); its turn state machine (ClaudeDriver.tla) and trace check change with it" :state :todo)
  (:id "t-accounts-sign-in" :type :task :title "5c. Sign-in: each provider's login flow through the secret dialog; the server reports each account signed in or needing sign-in, and rotation picks only signed-in accounts" :state :todo)
  (:id "t-accounts-drivers" :type :task :title "5d. Drivers take the session's account (the environment each driver starts with) in place of the drivers file's --account, and tell their AI where the config lives and its verbs (configBrief); each driver reports a usage limit as the protocol's `usage-limit` error (the Claude driver now reports a rate limit as `harness`; its model, ClaudeDriver.tla, changes with it), so that the server rotates the session; a driver per provider; a switch the harness cannot resume, within or across providers, starts fresh seeded with a summary of the conversation so far" :state :todo)
  (:id "t-accounts-window" :type :task :title "5e. Window: Command-A selects a session's account from its friend's, or leaves the choice to the server's rotation; several accounts of one friend may run at once" :state :todo)
  (:id "t-accounts-remote-instance" :type :task :title "5f. A driver on the instance's machine: a session whose instance is on another machine than the server's gets its driver started there, in the instance's directory, over the fleet's ssh by the machine's name (today it gets a notice and no driver, and a session whose instance moves to another machine is told and its driver ends once no turn is open). Moving an instance within the server's machine is done: the driver restarts in the new directory, resuming there when it is the same directory, else starting afresh seeded with a summary of the conversation so far; on another machine the same, once the driver runs there" :state :todo)
  (:id "t-accounts-friend-limit" :type :task :title "5g. A friend limited to its directory: friends.json's \"limit\" (true for a friend added, false only by an edit), accounts.json's \"billing\" (a friend on an API-billed account is always limited), decided by the server at every driver start and passed as --limit cwd; the Claude driver enforces it with Claude's sandbox, Claude's working-directory limit and the hook, and refuses a start it cannot limit" :state :done
   :evidence ("models/Accounts.tla: ctest -L model tlc_accounts_limit tlc_accounts_witness_start_ignores_friend tlc_accounts_witness_start_ignores_billing tlc_accounts_witness_link_api_unlimited tlc_accounts_witness_add_friend_open tlc_accounts_witness_limit_silent" "tests/accounts/accounts_walk.cpp: ctest -L model tlc_accounts_trace_* (tlc_accounts_trace_faulty_limit_rejected)" "tests/accounts/accounts_test.cpp: ctest -L unit accounts_unit_limits" "tests/server/supervisor_test.cpp: ctest -L unit supervisor_unit_limit" "drivers/claude/test/unit: npm test (limitOf, the hook's limit, prepare's refusals, --limit)" "drivers/claude/test/functional/wall.test.ts: NEXUS_WALL_RUN=limited and limited-nohook, on macOS, Ubuntu and WSL2 in CI"))
  (:id "t-friends-window" :type :task :title "6. Window: a friend tab shows the conversation" :state :todo)
  ; Feedback for Prax's author (a gap nexus works around, AGENTS.md): no table renders it.
  (:id "prax-json-strict-utf8" :type :task :title "Prax feedback: a strict mode for lib/json and lib/core's UTF-8" :state :todo
   :note "prJsonParse reads ill-formed UTF-8 inside strings, and a \\u escape of a lone surrogate, as U+FFFD without an error; prUtf8Validate accepts encoded surrogates; neither has a strict option. nexus checks each driver protocol line itself (src/driver/protocol.cpp illFormedAt, loneSurrogateAt). Confirmed by a second reader on 2026-11-10.")
  (:id "prax-fixed-mask" :type :task :title "Prax feedback: a password field whose mask does not show the length" :state :todo
   :note "Prax's password edit (lib/ui/src/ui_text_edit.cpp) shows one U+2022 for each code point, so the dialog shows how long a secret is; docs/secrets.md wants a fixed row of dots whatever the length. Not yet checked by a second reader; that comes before it goes to Prax's author.")
  (:id "prax-wipe-password-buffers" :type :task :title "Prax feedback: a password field that overwrites its edit buffers when its text is cleared or replaced, and when it is destroyed" :state :todo
   :note "Prax's password edit (lib/ui/src/ui_text_edit.cpp) keeps a password out of the clipboard but frees its text without overwriting it, so the secret dialog's value stays in the window's memory until it is reused (docs/secrets.md, The dialog). nexus overwrites its own copies. Not yet checked by a second reader; that comes before it goes to Prax's author.")
  (:id "secrets-walk-drivers" :type :task :title "The secret dialog's trace walk drives the real Drivers too, over a fake harness, so each secret line a driver's standard input receives is in the trace's records" :state :todo
   :note "tests/sync/secrets_walk binds AccountFiles and SecretLines today (docs/secrets.md, Binding and leak tests). Drivers needs driver processes, a poll loop and real time per asker; a fake harness standing in for them inside the walk is about 400 to 600 lines. Until then link_e2e secret and drivers_functional secret_ask guard it.")
  (:id "login-life-codex" :type :task :title "A codex account's login lives by itself: the Codex CLI's ChatGPT sign-in (auth.json) is refreshed by the CLI and its refresh token rotates, so nexus stores it once (through the secret dialog, or codex login --device-auth on the server) and lets the CLI keep it; a refresh the provider refuses asks again, saying why (docs/accounts.md, How long a login lasts)" :state :todo
   :note "Waits on a codex driver. Sources: the Codex authentication docs; openai/codex codex-rs/core/src/auth.rs (refresh about every 8 days and on a 401).")
  (:id "login-life-gemini" :type :task :title "A gemini account's login lives by itself: the Gemini CLI's Google sign-in (oauth_creds.json) is refreshed unattended from its refresh token, so nexus stores it once and lets google-auth-library keep it; a refresh the provider refuses asks again, saying why" :state :todo
   :note "Waits on a gemini driver. Sources: geminicli.com/docs/get-started/authentication; google-gemini/gemini-cli packages/core/src/code_assist/oauth2.ts (access tokens about an hour).")
  (:id "login-life-api-keys" :type :task :title "API keys (Anthropic Console, OpenAI, Gemini, xAI) last until revoked: stored once, never asked again but on a 401 the provider gives, saying so; an xAI key's optional expiry is read once and noticed 30, 7 and 1 days ahead as a claude login's year is" :state :todo
   :note "Waits on a provider of nexus that stores a key. Source for the optional expiry: docs.x.ai.")
  (:id "t-heads-down" :type :task :title "Heads-down: server-side timer and queue, stop always gets through, one away-report" :state :todo)
  (:id "t-fleet" :type :task :title "Fleet: the machines and what runs on each" :state :todo)
  (:id "t-machines-from-server" :type :task :title "The fleet's machine list from the server, as the friends are: the server sends the machines it knows in its state (nova-config an optional source on the server's side), and the window stops reading nova-config for the terminal tabs' machine labels (src/remote.cpp, the client config's nova-config key), its last outside tool" :state :todo)
  (:id "t-sprint" :type :task :title "Sprint: see and deal cards in the window" :state :todo)
  (:id "t-panels" :type :task :title "Panels: data rendered by nexus, never code loaded into it" :state :todo)
  (:id "t-one-app" :type :task :title "One app: nexus-server starts at boot and runs every tool and friend as its child; permissions granted once" :state :todo)
  (:id "t-ios-client" :type :task :title "iOS client: one universal iPhone and iPad app, a second client of the server (after layer 2; waits on Prax's iOS backend)" :state :todo)
  (:id "t-install-through-ai" :type :task :title "Install through AI: README as the installer, one friend (the installing AI), proven by a cold run on a clean Mac" :state :todo)
  (:id "t-real-isolation" :type :task :title "Real isolation: friends, API-billed fleet workers and CI workers in a sandboxed microVM, VM or container (local hypervisor, or GCP or AWS)" :state :todo)))
