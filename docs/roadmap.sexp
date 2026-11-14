; docs/roadmap.sexp: the data the tables in ROADMAP.md are generated from (nova-roadmap render).
; The layers of nexus, bottom up. Each is secured before the next starts.
(:schema 1
 :root "goal"
 :page (:roadmaps ("layers" "window-features" "configuration" "tab-kinds"))
 :nodes
 ((:id "goal" :type :work-set :children ("layers" "window-features" "configuration" "tab-kinds"))
  (:id "layers" :type :roadmap :title "Layers"
   :rows (
     ("window" "1. Window: tabs, transcript and prompt draw and take input (in progress)")
     ("server-sync" "2. Server and sync: a server holds tabs, transcripts and drafts and keeps running without a client; the client reconnects from anywhere to the current state; nothing typed is lost or applied twice (in progress)")
     ("friends" "3. Friends: each tab is a real conversation with one friend, reached through a driver process that is a child of the server (one per tab, docs/driver-protocol.md); nothing the server received is lost or shown twice; a prompt reaches the harness at most once; a turn cut by a driver's exit is shown as cut")
     ("heads-down" "4. Heads-down: timer and queue live on the server; visible queue, stop always gets through, one away-report at the end")
     ("fleet" "5. Fleet: see the machines and what runs on each")
     ("sprint" "6. Sprint: see and deal cards without leaving the window")
     ("panels" "7. Panels: friends add UI elements when asked; a panel is data (what to show, which tool's output feeds it, what each button sends) rendered by nexus, never code loaded into it")
     ("one-app" "Later: one app, nexus-server, starts at boot on the always-on machine and runs every tool and every friend as its own child process, so permissions are granted once to that app and nothing else runs as a separate app or daemon")
     ("ios-client" "iOS client (one universal app for iPhone and iPad), a second client of the server; after layer 2, waiting on Prax's iOS app backend" :later t)
     ("install-through-ai" "Install through AI, no installer: a person in their own AI, on their own subscription, points it at this repo; the AI builds nexus from source by README, installs nexus-server as a login item, and sets nexus up with one friend, its own harness and account. No sprint, no fleet. README is the installer and is proven by a cold run on a clean Mac. After the friends layer." :later t)
     )
   :columns (("done" "done"))
   :cells (
     ("window" "done" "t-window")
     ("server-sync" "done" "t-server-sync")
     ("friends" "done" "t-friends")
     ("heads-down" "done" "t-heads-down")
     ("fleet" "done" "t-fleet")
     ("sprint" "done" "t-sprint")
     ("panels" "done" "t-panels")
     ("one-app" "done" "t-one-app")
     ("ios-client" "done" "t-ios-client")
     ("install-through-ai" "done" "t-install-through-ai")
     ))
  (:id "window-features" :type :roadmap :title "Window features"
   :rows (
     ("terminal-tabs" "Terminal tabs: a shell per tab, after the friends; added with Cmd-T, closed with Cmd-W")
     ("sprint-tab" "Sprint tab: nova-sprint's board, always first and always Cmd-1; never moved, closed or typed into; the page opaque, as nova-sprint serves it")
     ("upload" "The plus button uploads a picked file to the server, into the selected AI's incoming/ directory; an image is also posted to the chat (needs layers 2 and 3)" :later t)
     ("upload-store" "The upload's storage on the server: each file a blob in the store by its content hash, sent in chunks that resume after a cut, and placed in the AI's incoming/ in one atomic step; waits for the upload design (an earlier draft is in the git history of src/store)" :later t)
     ("model-effort-keys" "Hotkeys to choose a friend's model and effort, in place of the model name that stood under the prompt" :later t)
     ("hdr" "HDR and wide gamut: the window asks Prax for its HDR swap chain (half floats, extended sRGB, EDR, gamut-mapped to the display) once Prax's display pass keeps alpha for a see-through window, and once nexus draws colours past sRGB or brighter than SDR white" :later t)
     )
   :columns (("done" "done"))
   :cells (
     ("terminal-tabs" "done" "t-terminal-tabs")
     ("sprint-tab" "done" "t-sprint-tab")
     ("upload" "done" "t-upload")
     ("upload-store" "done" "t-upload-store")
     ("model-effort-keys" "done" "t-model-effort-keys")
     ("hdr" "done" "t-hdr")
     ))
  (:id "configuration" :type :roadmap :title "Configuration through your AI"
   :rows (
     ("client-config" "Client config as data: ~/.nexus/client becomes documented, versioned, human-editable JSON in git, with the same verbs and apply report as the server's (docs/accounts.md); a person's AI changes it through the server, which relays the change to the client that asked and never stores, caches or syncs it: each machine's config is its own. Tab layout is client config too: a tab per friend or a tab per instance of a friend (the server sends every session's friend and instance names, and keeps no tabs of its own)")
     ("client-template" "A client-config template on the server, kept up to date: any change to the template becomes a pending update that each client applies at its next connect; config stays local to each machine, with no live sync. Open question: how a machine's local edits survive an update (proposal: an update replaces only the fields the template change touched)")
     ("client-promote" "Promote one machine's client config to the template, with a pending update for every other machine" :later t)
     ("ui-rules-to-config" "The window's rules that are one person's setup move into the client config: today's fixed zones (sprint, then friends, then terminals) become the default tab groups, in the default order dashboards, friends, terminals (tab-groups)")
     ("server-config-json" "The server's key-value config and its drivers file become documented JSON in nexus-config, with a version, check, reload and history like accounts.json")
     ("config-remote-signin" "Pushing the config repository to a remote stays opt-in (built: `nexus-server config remote`); later, an in-app GitHub sign-in for the pushes, through the secret dialog, in place of the git credentials of the user nexus-server runs as" :later t)
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
  (:id "t-client-config" :type :task :title "Client config as documented, versioned JSON in git, relayed by the server, never stored by it" :state :todo)
  (:id "t-client-template" :type :task :title "Client-config template on the server: pending updates applied at each client's next connect" :state :todo)
  (:id "t-client-promote" :type :task :title "Promote one machine's client config to the template" :state :todo)
  (:id "t-ui-rules-to-config" :type :task :title "Sprint tab at Command-1, friend tabs and terminals move into the client config" :state :todo)
  (:id "t-server-config-json" :type :task :title "Server key-value config and drivers file as documented JSON in nexus-config" :state :todo)
  (:id "t-config-remote-signin" :type :task :title "In-app GitHub sign-in for config pushes" :state :todo)
  (:id "t-config-history-pruning" :type :task :title "Config history pruning: whether and how" :state :todo)
  (:id "t-tab-kinds-data" :type :task :title "Tab kinds as data, chosen per machine in the client config" :state :todo)
  (:id "t-tab-groups" :type :task :title "Tab groups (dashboards, friends, terminals; later more) as client config, each with its own close, new-tab and drag rules" :state :todo)
  (:id "t-editor-tab":type :task :title "Editor tab after Sublime Text 3" :state :todo)
  (:id "t-files-tab" :type :task :title "File explorer tab" :state :todo)
  (:id "t-chat-tab" :type :task :title "Chat tab: a Chatto client" :state :todo)
  (:id "t-mail-tab" :type :task :title "Mail tab" :state :todo)
  (:id "t-social-tab" :type :task :title "Social tab" :state :todo)
  (:id "t-terminal-tabs" :type :task :title "Terminal tabs: a shell per tab after the friends" :state :done
   :evidence ("tests/window/test_terminal.cpp" "tests/model: tlc_witness_add_before_friend, tlc_trace_faulty_close_rejected"))
  (:id "t-sprint-tab" :type :task :title "Sprint tab: nova-sprint's board, first and Cmd-1, pinned" :state :doing)
  (:id "t-model-effort-keys" :type :task :title "Hotkeys to choose a friend's model and effort" :state :todo)
  (:id "t-hdr" :type :task :title "HDR and wide gamut: Prax's HDR swap chain, once its display pass keeps a see-through window's alpha" :state :todo)
  (:id "t-upload" :type :task :title "Upload: the plus button sends a file to the selected AI's incoming/, images to the chat too" :state :todo)
  (:id "t-upload-store" :type :task :title "Upload storage: blobs by content hash in the store, chunks that resume, atomic placement in incoming/" :state :todo)  (:id "t-window" :type :task :title "Window: tabs, transcript and prompt draw and take input" :state :doing)
  (:id "t-server-sync" :type :task :title "Server and sync: server holds the state; mosh-style sync over the tailnet; nothing typed lost or applied twice" :state :doing)
  (:id "t-wire-version" :type :task :title "Wire version: the window names the wire it speaks in its new request, and the server refuses another with which side to update (today an older window against a newer server fails its diffs)" :state :todo)
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
   :evidence ("models/Link.tla: ctest -L model tlc_link_sessions tlc_link_ensure tlc_link_witness_id_reuse tlc_link_witness_ensure_again tlc_link_witness_show_newest" "tests/sync/link_walk.cpp: ctest -L model tlc_link_trace_* tlc_link_trace_faulty_ensure_rejected" "models/Supervisor.tla: ctest -L model tlc_supervisor_switch tlc_supervisor_witness_setting_* tlc_supervisor_witness_account_* tlc_supervisor_trace_faulty_setting_rejected tlc_supervisor_trace_faulty_account_rejected" "tests/sync/sync_test.cpp: ctest -L unit sync_unit" "tests/store/store_test.cpp: ctest -L unit store_unit_ids store_unit_migrate" "tests/store/durable_test.cpp: ctest -L unit durable_unit_restart durable_unit_migrate" "tests/server/drivers_functional.cpp: ctest -L functional friends_functional_account" "tests/link/link_e2e.cpp: ctest -L functional link_e2e_*"))
  (:id "t-friends-claude" :type :task :title "4. Claude driver on the Agent SDK" :state :done
   :evidence ("drivers/claude/test/unit: npm test (scripted query: turns, stop, resume, session-not-found, settings, subscription refusals, protocol refusals)" "tests/driver/conformance_test.cpp: ctest -L claude_driver driver_conformance_claude_* (and -L functional driver_conformance_fake_*)" "tests/driver/protocol_test.cpp: ctest -L unit driver_unit_* (settings)" "drivers/claude/test/live/live.ts: npm run test:live, opt-in, on the subscription" "docs/driver-protocol.md: settings"))
  (:id "t-friends-claude-model" :type :task :title "Claude driver's turn state machine (credential check, push, stop and interrupt, settings, resume) in TLA+, bound by trace checks of the driver's unit walks" :state :done
   :evidence ("models/ClaudeDriver.tla: ctest -L model tlc_claude_driver tlc_claude_driver_witness_*" "drivers/claude/test/walk.ts, drivers/claude/test/unit/driver.test.ts: ctest -L model tlc_claude_driver_trace_*"))
  (:id "t-friends-accounts" :type :work-set :children ("t-accounts-server" "t-accounts-secret-dialog"
                                                       "t-accounts-sign-in" "t-accounts-drivers"
                                                       "t-accounts-window" "t-accounts-remote-instance"))
  (:id "t-accounts-server" :type :task :title "5a. Accounts and friends on the server: providers have accounts; friends link to any number of accounts of any providers, with a default; a friend has instances (a machine and a working directory each), and a session belongs to one of them; a session runs on one of its friend's accounts, set by the person or rotated by the server at a usage limit; accounts.json and friends.json in a git repository of their own, edited through the person's AI with `nexus-server config check`, `reload`, `history`, `restore` and `remote`" :state :done
   :evidence ("models/Accounts.tla: ctest -L model tlc_accounts tlc_accounts_witness_*" "tests/accounts/accounts_walk.cpp: ctest -L model tlc_accounts_trace_*" "tests/accounts/accounts_test.cpp: ctest -L unit accounts_unit_*" "tests/accounts/config_repo_test.cpp: ctest -L functional accounts_functional_config_repo"))
  (:id "t-accounts-secret-dialog" :type :task :title "5b. Secret dialog: the window's own dialog for a credential, sent to the server and written 0600 under ~/.nexus/server/accounts/<name>/; never shown, logged, or seen by an AI" :state :todo)
  (:id "t-accounts-sign-in" :type :task :title "5c. Sign-in: each provider's login flow through the secret dialog; the server reports each account signed in or needing sign-in, and rotation picks only signed-in accounts" :state :todo)
  (:id "t-accounts-drivers" :type :task :title "5d. Drivers take the session's account (the environment each driver starts with) in place of the drivers file's --account, and tell their AI where the config lives and its verbs (configBrief); each driver reports a usage limit as the protocol's `usage-limit` error (the Claude driver now reports a rate limit as `harness`; its model, ClaudeDriver.tla, changes with it), so that the server rotates the session; a driver per provider; a switch the harness cannot resume, within or across providers, starts fresh seeded with a summary of the conversation so far" :state :todo)
  (:id "t-accounts-window" :type :task :title "5e. Window: Command-A selects a session's account from its friend's, or leaves the choice to the server's rotation; several accounts of one friend may run at once" :state :todo)
  (:id "t-accounts-remote-instance" :type :task :title "5f. A driver on the instance's machine: a session whose instance is on another machine than the server's gets its driver started there, in the instance's directory, over the fleet's ssh by the machine's name (today it gets a notice and no driver). Moving an instance (an edit of its machine or directory): the next session starts there; a running session's driver restarts there, resuming the conversation when the directory is the same repository, else starting fresh seeded with a summary of the conversation so far" :state :todo)
  (:id "t-friends-window" :type :task :title "6. Window: a friend tab shows the conversation" :state :todo)
  ; Feedback for Prax's author (a gap nexus works around, AGENTS.md): no table renders it.
  (:id "prax-json-strict-utf8" :type :task :title "Prax feedback: a strict mode for lib/json and lib/core's UTF-8" :state :todo
   :note "prJsonParse reads ill-formed UTF-8 inside strings, and a \\u escape of a lone surrogate, as U+FFFD without an error; prUtf8Validate accepts encoded surrogates; neither has a strict option. nexus checks each driver protocol line itself (src/driver/protocol.cpp illFormedAt, loneSurrogateAt). Confirmed by a second reader on 2026-11-10.")
  (:id "t-heads-down" :type :task :title "Heads-down: server-side timer and queue, stop always gets through, one away-report" :state :todo)
  (:id "t-fleet" :type :task :title "Fleet: the machines and what runs on each" :state :todo)
  (:id "t-sprint" :type :task :title "Sprint: see and deal cards in the window" :state :todo)
  (:id "t-panels" :type :task :title "Panels: data rendered by nexus, never code loaded into it" :state :todo)
  (:id "t-one-app" :type :task :title "One app: nexus-server starts at boot and runs every tool and friend as its child; permissions granted once" :state :todo)
  (:id "t-ios-client" :type :task :title "iOS client: one universal iPhone and iPad app, a second client of the server (after layer 2; waits on Prax's iOS backend)" :state :todo)
  (:id "t-install-through-ai" :type :task :title "Install through AI: README as the installer, one friend (the installing AI), proven by a cold run on a clean Mac" :state :todo)))
