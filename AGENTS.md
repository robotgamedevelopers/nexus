# AGENTS.md

The rules for an AI working in this repository. Each one holds on every change.

## Code

- nexus is C++ built with CMake. Build with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` and `cmake --build build -j "$(sysctl -n hw.ncpu)"`.
- Zero warnings. Warnings are errors on every compiler; a change that adds a warning does not land.
- Prax is pinned and never forked or patched. When nexus needs something Prax lacks, work around it in nexus and record the gap as feedback for Prax's author.
- Errors never fail silently. Each error carries its cause and how to fix it.

- Use Prax fully: take the highest Prax layer that does the job, and never rewrite what Prax already has.
- mosh's transport is used unmodified (fetched at mosh-1.4.0) for the link between the window and `nexus-server`.
- An AI working here never launches the app's window, takes screenshots of it, or kills a running nexus process; the person running nexus does that.

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
- Command-W closes the tab showing if it is a terminal, and the tab selected before it shows again. On the sprint tab or a friend's tab, Command-W does nothing. A terminal whose shell exits closes the same way.
- A terminal tab that has ssh'd into a fleet machine is labelled with that machine's name for the whole ssh session.
- Command-plus (with or without Shift) and Command-minus change the text scale for every tab at once, in steps of 0.1 from 0.5 upward. Everything scales together: text, spacing and radii.
- These persist across relaunch: the text scale, the friends' order, the selected tab (the sprint tab if that tab is gone), and the open terminals. Terminals come back in their order, each a new login shell in the directory its shell last used.

Friend tabs
- Enter sends, Command-Enter queues, and Shift-Enter breaks the line.
- Command-Escape stops the friend's work.
- With `--server`, sending and queuing go through `nexus-server`, and the server's state is what the tab shows. Before the server is reached, the prompt is replaced by one plain line giving the link's state.

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
- HDR is on the roadmap. It is blocked because Prax's HDR display pass drops alpha, which the transparent overlay needs.

## Tests

- A unit test uses no real time, no sockets, no network and no outside services. It takes time as an argument and fakes the transport. Anything that needs the real thing is a functional test.
- `ctest -L unit` runs the unit tests; `ctest -L functional` the functional tests (the headless window, which sets text in the system's fonts, real processes, real files, and the link end to end: `nexus-server` over `ssh localhost` and mosh's transport over real UDP, never ssh to another machine); `ctest -L model` the TLA+ models and their trace checks. CI runs all three on every pull request; the largest TLC instances (label `model_nightly`: the window model's three largest, the link's and the store's largest) run in the nightly workflow, and every property is still checked on every pull request on the smaller ones.
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
