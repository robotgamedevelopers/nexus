# AGENTS.md

The rules for an AI working in this repository. Each one holds on every change.

## The design principle (check every change against it)

1. **Everything is data.** How a person works (accounts, friends, which tabs show, keys, boards) is documented JSON in
   git, never compiled in. nexus fixes only what it must: the server, the rendering, and safety.
2. **The person's AI is the configurator.** There are no settings screens: a person changes nexus by talking with their
   AI. The documented schema (`docs/accounts.md`, and each file's page after it) and the server's error text are the
   AI's whole interface, so each says enough to act on without reading code.
3. **The server is the authority.** It validates, applies or refuses with the reason, and keeps the history. Nothing
   breaks and everything can be undone.
4. **Extension is data too.** New tab kinds and panels are described as data that nexus renders (`docs/roadmap.sexp`,
   the tab-kinds rows).
5. **Secrets are the one exception.** A credential enters only through the in-app secret dialog or sign-in, never
   through the AI, and is never shown, logged or put in a file the AI reads.

The sprint tab, the friends and the terminals are one person's configuration, not hardwired; the locked rules below
that fix them (such as the sprint tab pinned at Command-1) move into configuration with the client-config work.
All rendering is Prax, at its highest layer that does the job: nexus never hand-rolls UI that Prax has, and a gap in
Prax goes to Prax's author as a request, never a fork.

## Code

- nexus is C++ built with CMake. Build with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` and `cmake --build build -j "$(sysctl -n hw.ncpu)"`.
- Zero warnings. Warnings are errors on every compiler; a change that adds a warning does not land.
- Prax is pinned and never forked or patched. When nexus needs something Prax lacks, work around it in nexus and record the gap as feedback for Prax's author.
- Errors never fail silently. Each error carries its cause and how to fix it.
- Make decisions on the server. `nexus-server` is where the real work runs: anything that affects the state of the AI models (sessions, accounts, models, effort, transcripts, queues, session logs) lives and is decided there, against the server's state when it processes each message. The client is a thin client, a portal: it sends inputs and events and displays what comes back. Its only state is what is being prepared to send (text typed with no latency) and its own visual-only preferences (tab order, the selected tab, scroll position, text scale). So behaviour never depends on latency, and nothing here is a distributed-programming problem.
- The server and the wire know sessions and accounts, never tabs or any other part of the UI. How sessions are shown (tabs, subtabs, several per tab, one in two windows) is the client's business alone.

- Everything nexus keeps lives under ~/.nexus: ~/.nexus/server and ~/.nexus/client, each with its own config. Every token and key is a 0600 file there; none in the Keychain, the environment or another tool's directory. No exceptions.
  - `~/.nexus/server/` (`src/server/home.h`): `config`, `drivers`, `store/`, `server.sock`, `server.log`, `nexus-config/` (the accounts and friends, `accounts.json` and `friends.json`, in a git repository of their own: `docs/accounts.md`), and `accounts/<name>/`, one harness login each (the Claude driver's `--account`; its token is `accounts/<name>/oauth-token`), outside `nexus-config/` so that no credential is ever in its history.
  - `~/.nexus/client/` (`src/client_home.h`): `config` (`nova-config <path>`, `sprint-url <url>`), the window's saved preferences, and on Windows `nexus.log`.
  - Each side resolves its home in one function (`serverHome`, `clientHome`: HOME, or USERPROFILE for the native Windows window) and reads nothing nexus-specific from the environment. On Windows the native window's `%USERPROFILE%\.nexus\client` and the WSL2 server's `$HOME/.nexus/server` are different directories on purpose.

- Use Prax fully: take the highest Prax layer that does the job, and never rewrite what Prax already has.
- mosh's transport is used unmodified (fetched at mosh-1.4.0) for the link between the window and `nexus-server`.
- An AI working here never launches the app's window, takes screenshots of it, or kills a running nexus process; the person running nexus does that.
- `nexus-server` reaches each friend through a driver, a child process per session, over the driver protocol: versioned and documented in `docs/driver-protocol.md`, which changes in the same pull request as `src/driver/protocol.h`. A change an older reader cannot skip raises `kProtocolVersion`.
- A window and `nexus-server` never have to be the same version, and a server reads every store an older one left (`docs/formats.md`, which changes in the same pull request as `src/sync/formats.h` or a codec). The state and the store frame each record that can grow by its length; a later version only appends fields; a field an older writer lacks reads the default that claims least; an unknown entry kind shows as a notice, an unknown activity as busy. A window and a server agree on the older of their state formats when the window connects, and refuse only a format declared incompatible (none today), in one plain line. A store record or snapshot is refused only when it declares a reader newer than this build. The fixtures of main's formats are bytes main's code wrote, tested old to new and new to old (`sync_unit`, `store_unit_formats`, `link_e2e_older_server`).
- `nexus --screenshot --state FILE` draws from the state file alone: no home, no config, no nova-config, no saved tab order, no sprint board; its friend tabs are the friends of the state's sessions (`window_state_screenshot`).
- A driver holds no state the server cannot rebuild: the server keeps the turn numbers and the session id, and starts a driver again with `--resume`.
- A prompt is written to a driver at most once. A turn left open when its driver exits or the server restarts is cut with a notice and never sent again (`models/Supervisor.tla`). Which program is each friend's driver comes from the file `drivers` in the server's directory (`src/server/driver_table.h`).

## How nexus behaves (locked: the owner's decisions; every change keeps all of them)

Each rule here has a test or a model behind it. A change that breaks one is a regression, not a redesign. Only the owner changes a rule, and the change is recorded here in the same pull request.

Launch and the overlay
- nexus is an overlay: a borderless panel above other apps, on every Space and beside fullscreen apps. It has a see-through backdrop, covers the whole screen including the menu bar, and runs as an Accessory app with no Dock icon. `--windowed` gives an ordinary opaque window instead, in which Escape does nothing.
- At launch and relaunch nothing shows: no window, no single frame, and no change of focus. nexus connects to `nexus-server` in the background, and only the hotkey shows it. The one exception is a hotkey that fails to register, because then nothing else could show nexus.
- The hotkey is Command-backtick, the key left of 1, chosen by key position. It hides an overlay that is visible and has the keyboard, and shows it in every other case: after Command-Tab, the next press shows it rather than merely reactivating it.
- An overlay that had the keyboard and lost it (to Command-Tab or a click on another app) hides. One that never got the keyboard after a show is hidden at the key check rather than left standing over other apps.
- Escape hides the overlay on the sprint tab and on a friend's tab. On a terminal tab, Escape always goes to the terminal: vim, shells and agents need it.

Tabs
- The strip holds three zones, in this order: the sprint tab alone, then the friends, then the terminals. The sprint tab is always first and never moves or closes.
- Friend tabs come from the friend list and never close.
- Terminal tabs come and go, and their saved order is never written.
- A tab is dragged to reorder it, only within its own zone. Everything it holds goes with it, and the selection follows it.
- The friends' order is saved after every drag and restored at launch. Friends the saved order doesn't name follow in list order, and names that no longer exist are dropped.
- Command-1 to Command-0 select tabs 1 to 10 in the strip's current order. Command-1 is the sprint tab, and pressing it also takes the board back home. A key with no tab does nothing.
- Command-T opens a terminal tab at the end, running the login shell in the home directory, and shows it. A terminal that cannot start leaves the previous tab showing.
- Command-W closes the tab showing if it is a terminal, and the tab selected before it shows again. On the sprint tab or a friend's tab, Command-W does nothing. Exiting the shell (`exit`, Control-D) does the same: the tab closes and the tab selected before it shows again.
- A terminal tab that has ssh'd into a fleet machine is labelled with that machine's name for the whole ssh session.
- Command-plus (with or without Shift) and Command-minus change the text scale for every tab at once, in steps of 0.1 from 0.5 upward. Everything scales together: text, spacing and radii.
- These persist across relaunch: the text scale, the friends' order, the selected tab (the sprint tab if that tab is gone), and the open terminals. Terminals come back in their order, each a new login shell in the directory its shell last used.

Friend tabs
- Enter sends, Command-Enter queues, and Shift-Enter breaks the line.
- Command-Escape stops the friend's work. Escape alone does not stop: it leaves the innermost context, and with none open hides the overlay.
- With `--server`, sending, queuing and stopping go through `nexus-server`, and the server's state is what the tab shows. Before the server is reached, the prompt is replaced by one plain line giving the link's state.
- Nothing stands under or around the prompt but its box: no labels, hints or placeholder text (a tool for experts). A link lost once the prompt is open shows in the state cursor, and why a prompt was not sent, or how much text was cut to fit it, shows at the end of the transcript (`window_input` `nothingAroundTheOpenPrompt`, `cutLineStaysWithItsTabAndDraft`).
- The friend's reply streams: its text shows after the transcript as it grows, and joins the transcript, closed, when its block ends (`LiveSound` and `EachClosedOnce` in `models/Link.tla`; `window_input` `growingTextShowsAfterTheTranscript`).
- Each tool call is one compact line in the transcript, the tool and a short summary of what it does, never wrapped and with nothing to expand; tool lines in a row stand close together (`window_input` `toolLinesAreOneCompactLine`).

Stopping
- Command-Escape always sends a stop to the server, whatever the window shows (no turn seen, or the link down: then it waits in order with the other actions). The client never filters a stop (`sync_unit` `stop_never_filtered_by_the_window`).
- Stops arrive in order. Each one stops the friend's work running when the server processes it, an open turn or a driver starting one, and the next queued prompt then goes. A stop that finds nothing running is dropped and never affects a later turn, so pressing it repeatedly is always safe (`StopEndsWhatRuns` and `NoLatchedStop` in `models/Supervisor.tla`).

The state cursor
- Under the last text in a friend's tab, on its own line, a cursor shows the session's state, from the server's state only: pulsing light blue while a turn is open (starting and stopping included); steady dark blue when it is waiting for the person; steady red when the friend is down (its driver crashed, keeps failing to start, or has no login, or the friend has no driver configured: the drivers file names none, from the session's making on, until the file gives one, which the server reads again each second while a friend has none); dim grey when the window has lost the server (never blue then). Colours are OKLCH; the pulse is a slow brightness breathe, about 1.5 s (`src/cursor.h`). Nothing shows the queue or anything else there. The server works it out (`ActivityOf`, `IdleWaitsForPerson`, `CrashShowsDown`, `DownLeavesByWork` and `NoDriverShowsDown` in `models/Supervisor.tla`, bound to the code by its trace); the window shows it (`sync_unit` `cursor_from_the_servers_state`, `window_input` `stateCursorUnderTheLastText`).

Sessions
- A session has an id, a harness (Claude for now), an account, a model and an effort, plus its transcript, its queue and its driver. `nexus-server` mints the id: from 1, durable, and never reused, across restarts (`IdsNeverReused` and `SessionsKept` in `models/Link.tla`; `store_unit_ids`). The store, the wire and the drivers are keyed by session id, never by friend name or by tab. A session is never removed; its friend is one of its attributes.
- Several sessions may be with one friend, and share a harness and an account, or not. How they are used is still to be decided; the server keeps it possible.
- A window asks and the server decides. A create makes a session with a friend, copying the account, model and effort of the session it names, or taking the friend's defaults (`hub_create_copies_settings`); an ensure makes one only for a friend that has none (`EnsureMakesOne`). A create or ensure past the limit on sessions, or a send, queue or stop naming a session the server does not hold, is refused and counted, and makes nothing.
- A change of a session's model or effort applies from its next turn: a turn never changes model halfway (`WrittenWithSetting` and `SettingLeavesTurn` in `models/Supervisor.tla`). The driver is told them by a `settings` line before the send that first runs with them; a field put back to the harness's default, which that line cannot say, ends the driver once no turn is open, and the next starts on the defaults (`WSupSettingNotCleared`). A change of its account keeps its id and its transcript, and ends its driver once no turn is open, so that the next starts for the new account (`NoSendOnOldAccount`, `AccountKeepsTurn`; `friends_functional_account`). Until the drivers take the account themselves (part 4 of the sign-in work, `docs/roadmap.sexp`), the server puts a session's account in place of the `--account` value of its friend's line in the `drivers` file; a line without `--account` starts as it is; a session on no account starts no driver whose line takes one, and a prompt in a session on an account whose provider has no driver yet is not sent, each with the reason and the fix. A server from before accounts takes its first accounts from those `--account` values, so that no session moves.
- A store from before sessions (keyed by friend tab) opens as one session per friend, transcripts, queues and drivers intact, and is written in the sessions format from then on (`store_unit_migrate`, `durable_unit_migrate`).
- The window asks for a session with each friend at launch, and each friend tab shows its friend's first session (`ShowsFirst`). Which session a tab shows is the window's own state, never the server's.

Accounts and friends (`models/Accounts.tla`, `src/accounts/registry.h`, `docs/accounts.md`)
- Accounts are their own thing, set up on their own: providers have accounts, each account one provider. Friends are separate, and link to accounts: any number, of any providers, in an order, with one of them the friend's default or none. An account may serve any number of friends; nothing restricts how a person chooses to work (`LinksSound`, `DefaultLinked`).
- A friend is an identity (its name, its linked accounts, its default account); an instance is that friend on one machine in one working directory (owner, 2026-11-14: "'studio' '/Volumes/nova/ai/rowan' is you here", "this is an instance of you"). A friend has any number of instances in friends.json (`"instances"`, each a name unique within the friend, a `machine` host name and an absolute `dir`; the first is the default), set by edits like any other field. A session belongs to one instance (`SessionInfo::instance`, in the store and on the wire, so a window may group tabs per friend or per instance as its own config says); accounts are the friend's, shared by its instances. A session made copies the instance of the session it copies when it is the same friend's, else takes the default; an edit that removes a session's instance moves it to the default, told (`InstanceHeld`, `MovedIsTold`). Its driver works in the instance's `dir`; an instance on another machine than the server's gets a "not yet" notice and no driver (running it there over the fleet's ssh is a roadmap row).
- A session runs on one of its friend's linked accounts, or on none, and runs its account's provider's harness (`SessionLinked`, `HarnessMatches`). A new session takes the account of the session it copies when that is linked to its friend, else the friend's default. The person may set a session's account to any of its friend's (`SetExact`); one not linked is refused with a notice that says how to link it.
- The server rotates a session at its account's usage limit to its friend's next signed-in linked account, in link order (`RotateMoves`), or tells it there is none. The person can also choose. Several of a friend's accounts may run at once, in different sessions. Sign-in is the server's own state: an account is signed in at the start when its login is there, and after that as its drivers report (a credential, no credential, a usage limit).
- No change ever leaves a session silently on an account no longer linked to its friend: an unlink, a removal or an edit moves each such session in the same step to the friend's default when signed in, else its first signed-in linked account, else none, and tells it (`MovedIsTold`). An account the server picks is always signed in (`ServerPicksSignedIn`). A refused change changes nothing (`RefusedChangesNothing`).
- A switch of account, within a provider or across providers, need not resume the harness's session: the driver may start fresh, seeded with a summary (part 5 of the sign-in work).
- The accounts and friends are two pretty-printed JSON files, `accounts.json` and `friends.json`, documented field by field in `docs/accounts.md`, each with a `"version"`. A file of an older version is upgraded in place, entries kept, and committed as an upgrade; a newer version is refused with a message and left as it is, never wiped. Credentials never enter them.
- Configuration is by talking with your AI: the AI edits the files, `nexus-server config check` (changes nothing; every error with file, line and fix) and `nexus-server config reload` (applies the edit whole or refuses it whole, and prints and logs each line applied or refused). The server decides every change when it applies it; a reload is explicit, so a half-finished edit is never applied. The commands work over ssh from another machine.
- The files live in their own git repository, `~/.nexus/server/nexus-config/`: every change applied is one commit saying what changed and who asked; `config history` lists them and `config restore <commit>` brings one back as a new commit; history is never rewritten. Only its three files are ever tracked, every commit names only the two JSON files, and nothing writes over hand edits not yet applied (a restore is refused and an upgrade waits). git runs with a clean environment: never the person's git config, never a prompt, never above its directory, a deadline on every call. With `config remote <url>` (a private repository of the person's, suggested `<owner>/nexus-config`) each commit is pushed in the background, a child the server's loop watches with a deadline, never in the loop: a push never blocks a reply or undoes a change, and a history that ever held another file is never pushed.

The sprint tab
- It keeps nova-sprint's original branding and logo. The app itself is called "nexus" (its bundle display name), but the board is not rebranded.
- The board's link to GitHub is hidden. Any link that leaves the board opens in the system browser; the board has no in-tab history.
- The board must show everything it shows in a browser, the cost pie chart included. (Not yet true: the pie chart is blank in nexus.)

Scrolling (like Safari on macOS)
- Up and Down scroll for as long as the key is held, driven by key-down and key-up, never by key repeat. A tap moves one line. A held key scrolls at `kHoldPixelsPerSecond` (2000 px/s) and starts after `kHoldTapSeconds` (80 ms).
- Page Up and Page Down move a page less one line and glide. A wheel notch glides one line. Command-Up/Down, Home and End glide to the ends.
- Each glide has momentum and eases to rest. A key at an end bounces past it and springs back, rubber-banded on Apple's curve.
- The OS's key repeats never reach the page. A trackpad's own pixels and momentum pass straight through.
- A click or a trackpad scroll stops a glide where the page is.
- The tuning lives in `src/scroll_glide.h`. When it changes, the owner's request is cited there.

Display
- nexus draws at the display's refresh rate (120 Hz on ProMotion).
- The client runs like a game, at 120 Hz: each frame's main-thread work fits in 8.3 ms (the display's own frame time where Prax reports its refresh rate). Anything that waits, on a disk, a socket, a process, a lock held by another thread or a slow system call, runs asynchronously off the frame thread: shells start and end on the terminals' thread, the sprint board's page runs on its own thread, the link on its own, and preference files are written by a writer thread. A hitch is a bug, fixed by moving the wait off the frame thread, never by loosening the budget (owner, 2026-11-14). The hitch detector (`src/hitch.h`, `docs/frame-budget.md`) is always on and logs every frame over budget, naming its phases; the frame-budget test (`window_frame_budget`, `tests/window/test_frames.cpp`) drives the window through stalls, long transcripts, a big paste, tabs, terminals, the sprint tab, text scale and resizing, and fails on any frame over budget in each of three runs (a real hitch recurs on the same frame; a host's scheduling does not), naming the frame, the phase, and each run's wall-clock and thread CPU time; the budget is the same in every run, a window's first frame keeps 4 ms (half the budget, for a host at half speed), and `--inject` proves a 10 ms hitch fails every time. A case may be marked known only while its fix is outside nexus (a Prax request), and then only the frames that hitch for that reason, each under a ceiling of the worst measured and about 15%; every other frame of the case keeps the budget.
- HDR is on the roadmap. It is blocked because Prax's HDR display pass drops alpha, which the transparent overlay needs.

## Decided by the owner, not yet built (2026-11-11)

Each of these moves into the locked rules above, with its test or model, in the pull request that builds it. Until then it is the design to build, not a thing to redecide.

Installing
- There is no installer. nexus is installed through an AI: a person, in their own AI on their own subscription, points it at this repository, and the AI builds nexus from source by the README and sets it up. The README is the installer; every step says how to check it worked and what to do when it did not, and it is proven by a cold run on a clean Mac by an AI with no other context.
- A default install has one friend: the installing AI's own harness and account, so the person talks to that harness through nexus. No sprint tab and no fleet. Friends are configured in nexus itself; the fleet's friend list is an extra source, not a requirement.
- The first experience is: talk with my AI, and a quake-style terminal overlay. Nothing about sprints, fleets or boards shows until the person asks for it.
- Once the one friend works, the AI teaches the next steps, in this order and each only when the person wants it: running terminals in the overlay; adding more friends (more subscriptions); setting up a fleet over Tailscale; and last, a sprint, for API-rate work or going really wide.
- The sprint tab exists only when a board is configured; when it exists, it is Cmd-1, first, and never moves or closes.

Billing
- Friends run on subscriptions, always; a driver has no way to bill an API and refuses a session that is not on a subscription. API billing is for fleet workers only, where the fleet's record allows it.

Keys and contexts
- Everything is driven by the keyboard, and fast. Every action has a key; the mouse is never required, and dragging is only ever a second way. Pop-ups open at once, a repeat of the key steps to the next item, and typing filters the list.
- Every shortcut uses Command, never Control or Option.
- nexus is opinionated and makes trade-offs. When a key nexus wants clashes with a convention elsewhere (a text-editing key, a macOS shortcut), nexus takes it, says the cost in one line, and moves on. Inside the overlay, nexus is its own OS.
- The overlay acts exactly the same on every platform: macOS, Linux, Windows, iPad. Same keys, same contexts, same behaviour; only the code underneath differs. Command is chosen by position: the key directly left of the space bar. That is Command on macOS and iPad, and Alt on Windows and Linux keyboards (never Control, which terminals need), so the hotkey there is Alt-backtick. Where the OS or an app uses one of these keys, nexus takes it inside the overlay.
- Platform targets: macOS; Windows 64-bit only (x64); Linux on Wayland only, never X11, with the overlay as a wlr-layer-shell surface (KDE Plasma, Sway, Hyprland; on GNOME, which lacks it, nexus is an ordinary window) and the hotkey through the GlobalShortcuts portal; iPad.
- Windows: the overlay stays native, everything else WSL2 (Glenn, 2026-11-14). The only native Windows code is the overlay window; nexus-server, the link, the drivers and every test of them build and run as the Linux build inside WSL2, and nexus supports no native Windows build of anything else. The native window reaches nexus-server in WSL2 over a local socket. CI runs the functional tests inside WSL2 on captain (`.github/workflows/ci.yml`, job `wsl2-functional`).
- Terminal tabs on Windows are WSL2 shells, started in the WSL2 home directory: never cmd or PowerShell (Glenn, 2026-11-14).
- Escape always backs out one context: a pop-up, the session menu, a drag. The last context is the overlay itself, so only with nothing else open does Escape hide it. Terminal tabs still give Escape to the terminal.

Sessions
- A friend's tab holds that friend's sessions as subtabs, like a menu bar or a cross menu: friends run across the strip, and the selected friend's sessions drop down in a column under its tab. The column shows only while switching, and hides once a session is chosen.
- Command-Up/Down move between the sessions in the current friend's tab. (In a friend's tab this replaces Command-Up/Down gliding to the ends; Home and End still glide.)
- Command-N makes a new session with this friend as a subtab in this tab; Command-Shift-N makes it a tab of its own (like a browser's new tab and new window). A new session starts with the current session's account, model and effort.
- A session can be dragged out of the column into the strip to become its own tab.
- Switching a session's account happens inside its tab. When the harness cannot resume across accounts, the driver starts fresh, seeded with a summary of the conversation so far (part 5 of the friends work, `docs/roadmap.sexp`).
- Command-M opens a pop-up selector of the harness's models and Command-E one of its effort levels. Up/Down move, Enter picks, Escape closes it. Nothing shows the model or effort otherwise.

Tabs
- Command-Left/Right select the tab to the left or right: navigation, on every tab. They no longer move to the start or end of a line in the prompt, which is a single line. With Command-Up/Down for sessions, the arrows make one grid: friends across, sessions down.

## Tests

- A unit test uses no real time, no sockets, no network and no outside services. It takes time as an argument and fakes the transport. Anything that needs the real thing is a functional test.
- `ctest -L unit` runs the unit tests; `ctest -L functional` the functional tests (the headless window, which sets text in the system's fonts, real processes, real files, and the link end to end: `nexus-server` over `ssh localhost` and mosh's transport over real UDP, never ssh to another machine); `ctest -L model` the TLA+ models and their trace checks. CI runs all three on every pull request; the largest TLC instances (label `model_nightly`: the window model's three largest, the link's, the store's, the driver supervisor's largest and the Claude driver's with both settings) run in the nightly workflow, and every property is still checked on every pull request on the smaller ones.
- The Claude driver (`drivers/claude`, TypeScript) has its own: `npm run check` there (strict compile, lint with warnings as errors, unit tests over a scripted query), and `ctest -L claude_driver` the protocol's conformance cases against it (after `npm ci && npm run build`). Its turns are `models/ClaudeDriver.tla`, and `ctest -L model` checks the traces of its unit tests and of its walk (`test/walk.ts`) against it, so the model job needs Node and the driver built too. `npm run test:live` runs it on the real SDK and spends a subscription: opt-in, never in CI. Its SDK version is pinned exactly, with `package-lock.json` committed.
- Build with every core and run tests one job per core: `-j "$(sysctl -n hw.ncpu)"`.

## Models

- Every state machine has a TLA+ module beside it, written before or with the code.
- Each model has reversed witnesses: broken variants that TLC must reject.
- The code is bound to its model by trace checking in CI. A model the code is not checked against does not count.
- Client and server are separate state machines, apart from the transport between them, and each has its own TLA+ model.
- Where code does index arithmetic (rings, wrapping counters, reused slots), the model carries the same arithmetic.

## Layers

- nexus is built in layers, bottom up. Each layer is secured before the next one starts.
- The layer list lives in `docs/roadmap.sexp`, rendered to `docs/ROADMAP.md` by `nova-roadmap render`; run `nova-roadmap check` after any change to either.
- A tracer bullet is declared before it starts, runs end to end once, and then stops. What it taught is kept; its code is not the product.

## The repository

- There is one branch: `main`. Every change lands through a pull request that is merged as soon as its CI is green against current `main`. Delete the branch on merge, and never let work diverge on long-lived branches.

- The repository is private until it is released. Nothing about it is posted anywhere else.
- Future work and ideas go in `docs/roadmap.sexp`, never in GitHub issues.
- Commits use the shared agent identity, whose email is `305107455+rowan-claude@users.noreply.github.com`, set with repo-local `git config` only.
- No personal names anywhere in the repository: code, comments, docs or commit messages.
- License: GNU AGPL v3, permanently.
