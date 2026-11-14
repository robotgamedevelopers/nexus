; docs/roadmap.sexp: the data the tables in ROADMAP.md are generated from (nova-roadmap render).
; The layers of nexus, bottom up. Each is secured before the next starts.
(:schema 1
 :root "goal"
 :page (:roadmaps ("layers" "window-features"))
 :nodes
 ((:id "goal" :type :work-set :children ("layers" "window-features"))
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
  (:id "t-friends-accounts" :type :task :title "5. Accounts: the environment each session's driver starts with; when the harness cannot resume a session under a new account, its new driver starts fresh, seeded with a summary of the conversation so far" :state :todo)
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
