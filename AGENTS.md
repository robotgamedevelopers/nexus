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

## How nexus behaves (the owner's decisions; keep them on every change)

Launch and the overlay
- nexus runs as an overlay (an Accessory app, no Dock icon). At launch and relaunch nothing shows: no window, no single frame, and focus does not change. It connects to `nexus-server` in the background. Only the hotkey (Command-backtick) shows it. The one exception is a hotkey that fails to register, because then nothing else could show it.
- The hotkey hides an overlay that is visible and has the keyboard, and shows it in every other case. An overlay that had the keyboard and lost it (Command-Tab, a click on another app) hides.
- Escape hides the overlay in the sprint tab only. In a terminal tab, Escape always reaches the terminal, because vim, shells and agents need it. (Not yet true: Escape currently hides the overlay in every tab.)

Tabs
- The sprint tab comes first, labelled "sprint", on Command-1. Friends start at Command-2.
- Command-T opens a terminal. A terminal tab that has ssh'd into a fleet machine is labelled with that machine's name for as long as the ssh session lasts.
- Command-W returns to the tab that was selected before.
- Font size, open terminals and the selected tab persist across relaunch.

The sprint tab
- It keeps nova-sprint's original branding and logo. The app itself is called "nexus" (its bundle display name), but the board is not rebranded.
- The board's link to GitHub is hidden. Any link that leaves the board opens in the system browser; the board has no in-tab history.
- The board must show everything it shows in a browser, the cost pie chart included. (Not yet true: the pie chart is blank in nexus.)

Scrolling (like Safari on macOS)
- Up and Down scroll for as long as the key is held, driven by key-down and key-up, never by key repeat. A tap moves one line. A held key scrolls at `kHoldPixelsPerSecond` (2000 px/s) and starts after `kHoldTapSeconds` (80 ms).
- Page Up, Page Down, the wheel and the end keys glide with momentum. At an end the page springs back and is rubber-banded.
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
