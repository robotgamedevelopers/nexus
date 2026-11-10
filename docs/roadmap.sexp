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
     ("server-sync" "2. Server and sync: a server holds tabs, transcripts and drafts and keeps running without a client; the client reconnects from anywhere to the current state; nothing typed is lost or applied twice")
     ("friends" "3. Friends: each tab is a real conversation with one friend over nova-bus, driven by the server; no message lost or shown twice")
     ("heads-down" "4. Heads-down: timer and queue live on the server; visible queue, stop always gets through, one away-report at the end")
     ("fleet" "5. Fleet: see the machines and what runs on each")
     ("sprint" "6. Sprint: see and deal cards without leaving the window")
     ("panels" "7. Panels: friends add UI elements when asked; a panel is data (what to show, which tool's output feeds it, what each button sends) rendered by nexus, never code loaded into it")
     ("one-app" "Later: one app, nexus-server, starts at boot on the always-on machine and runs every tool and every friend as its own child process, so permissions are granted once to that app and nothing else runs as a separate app or daemon")
     ("ios-client" "iOS client (one universal app for iPhone and iPad), a second client of the server; after layer 2, waiting on Prax's iOS app backend" :later t)
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
     ))
  (:id "window-features" :type :roadmap :title "Window features"
   :rows (
     ("terminal-tabs" "Terminal tabs: a shell per tab, after the friends; added with Cmd-T, closed with Cmd-W")
     ("sprint-tab" "Sprint tab: nova-sprint's board, always first and always Cmd-1; never moved, closed or typed into; the page opaque, as nova-sprint serves it")
     ("upload" "The plus button uploads a picked file to the server, into the selected AI's incoming/ directory; an image is also posted to the chat (needs layers 2 and 3)" :later t)
     ("model-effort-keys" "Hotkeys to choose a friend's model and effort, in place of the model name that stood under the prompt" :later t)
     )
   :columns (("done" "done"))
   :cells (
     ("terminal-tabs" "done" "t-terminal-tabs")
     ("sprint-tab" "done" "t-sprint-tab")
     ("upload" "done" "t-upload")
     ("model-effort-keys" "done" "t-model-effort-keys")
     ))
  (:id "t-terminal-tabs" :type :task :title "Terminal tabs: a shell per tab after the friends" :state :done
   :evidence ("tests/window/test_terminal.cpp" "tests/model: tlc_witness_add_before_friend, tlc_trace_faulty_close_rejected"))
  (:id "t-sprint-tab" :type :task :title "Sprint tab: nova-sprint's board, first and Cmd-1, pinned" :state :doing)
  (:id "t-model-effort-keys" :type :task :title "Hotkeys to choose a friend's model and effort" :state :todo)
  (:id "t-upload" :type :task :title "Upload: the plus button sends a file to the selected AI's incoming/, images to the chat too" :state :todo)
  (:id "t-window" :type :task :title "Window: tabs, transcript and prompt draw and take input" :state :doing)
  (:id "t-server-sync" :type :task :title "Server and sync: server holds the state; mosh-style sync over the tailnet; nothing typed lost or applied twice" :state :todo)
  (:id "t-friends" :type :task :title "Friends: a real conversation per tab over nova-bus, driven by the server" :state :todo)
  (:id "t-heads-down" :type :task :title "Heads-down: server-side timer and queue, stop always gets through, one away-report" :state :todo)
  (:id "t-fleet" :type :task :title "Fleet: the machines and what runs on each" :state :todo)
  (:id "t-sprint" :type :task :title "Sprint: see and deal cards in the window" :state :todo)
  (:id "t-panels" :type :task :title "Panels: data rendered by nexus, never code loaded into it" :state :todo)
  (:id "t-one-app" :type :task :title "One app: nexus-server starts at boot and runs every tool and friend as its child; permissions granted once" :state :todo)
  (:id "t-ios-client" :type :task :title "iOS client: one universal iPhone and iPad app, a second client of the server (after layer 2; waits on Prax's iOS backend)" :state :todo)))
