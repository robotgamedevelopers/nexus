# nexus

nexus is a desktop app, being built, where one person talks with all of their AIs in one window: a tab for
each AI, and Cmd-1 to Cmd-0 to switch between the first ten of them. It is for people who trust their AIs. In
nexus each AI is to run with its permission prompts bypassed, inside a strict sandbox that is the guard, and
nexus will never show a permission prompt.

nexus is planned as two parts: a thin client, which is the window, and a server, for example on an
always-on machine, which holds the state and drives the work forward. The server keeps working when the
client is closed. The client syncs to the server's latest state over a tailnet, the way mosh does: over UDP,
surviving sleep and network changes, and echoing typing locally. None of this is built yet.

## A day with nexus

The day it is made for: about an hour of talking with the AIs and dealing out work, then away, then back later for another hour,
instead of sitting and waiting while they work.

## Heads-down mode

The person starts a timer and leaves. While it runs, anything they type is held in a queue the window
shows, and does not reach the AIs. A stop always gets through. When the timer ends, the queued text goes
out together with one away-report: what finished, what stopped and why, and what only the person can
decide.

The away-report can make trust checkable. It cannot create trust that is not there.

Heads-down mode is planned, not built.

## Status

Early, and not yet usable. macOS only. nexus is built in layers, one secured before the next starts, and
layer 1 (the window: tabs, transcript and prompt draw and take input) is in progress. The layers are in
[docs/ROADMAP.md](docs/ROADMAP.md).

## What it builds on

- [Prax](https://github.com/pdoane/prax), the UI library, pinned.
- [nova-tools](https://github.com/mas-bandwidth/nova-tools) and
  [nova-sprint](https://github.com/mas-bandwidth/nova-sprint): nexus calls their command-line tools. It runs
  no AI models itself.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

License: GNU AGPL v3.
