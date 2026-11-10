# nexus

nexus is a desktop app, being built, where one person talks with all of their AIs in one window: a tab for
each AI, and Cmd-1 to Cmd-0 to switch between the first ten of them. Cmd-T opens a terminal tab after the AIs'
tabs, a shell in the home directory, and Cmd-W closes it. It is for people who trust their AIs. In
nexus each AI is to run with its permission prompts bypassed, inside a strict sandbox that is the guard, and
nexus will never show a permission prompt.

nexus is planned as two parts: a thin client, which is the window, and a server, for example on an
always-on machine, which holds the state and drives the work forward. The server keeps working when the
client is closed. The client syncs to the server's latest state over a tailnet with mosh's own State
Synchronization Protocol: over UDP, surviving sleep and network changes. The first of it is built: see
[The server](#the-server).

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

Early, and not yet usable. macOS only. nexus is built in layers, one secured before the next starts: layer 1
(the window: tabs, transcript and prompt draw and take input) and layer 2 (the server, and the window syncing to it)
are in progress. The layers are in [docs/ROADMAP.md](docs/ROADMAP.md).

## The window today

nexus runs as an overlay: a see-through window over the whole screen, above other apps and their fullscreen
Spaces, with no Dock icon. Command plus the tilde key (the key left of 1) shows it and hides it, from any app.
Escape hides it too, and so does switching away: Command-Tab, a click on another app, or Mission Control. With
`--windowed` it is a normal window instead.

Each friend listed by `nova-config friend list` has a tab. Cmd-1 to Cmd-0 select the first ten. Drag a tab to
reorder the tabs; the order is kept across launches. Enter sends the prompt, Command-Enter queues it, and
Shift-Enter starts a new line. Cmd-Escape is stop. Cmd-plus and Cmd-minus make all text larger and smaller.

Cmd-T opens a terminal tab, the login shell in the home directory, after the friends' tabs; Cmd-W closes it, and it
closes by itself when the shell exits. Terminal tabs can be dragged among themselves, never in among the friends,
and their order is not kept. On a terminal tab every key that is not a Command chord, Escape included, goes to the
shell.

nexus finds `nova-config` on the PATH your login shell gives, or at the full path in `NEXUS_NOVA_CONFIG`.

## The server

`nexus --server <host>` is the window talking to `nexus-server` on another machine. The window runs
`ssh <host> nexus-server new` (through the login shell there, so `~/.local/bin` on its PATH is enough), which starts
the server if it is not running and prints one line, `NEXUS CONNECT <port> <key>`: a UDP port and a session key the
window keeps in memory and nowhere else. From then on the window and the server talk over UDP through mosh's
transport (AES-128-OCB, roaming, retransmission by diff); ssh is not used again. Until the window has the server's
first state it says it is connecting and takes no typing; after that it is quiet unless it has heard nothing from the
server for 6 s, when it says how long.

What syncs, so far: each prompt sent or queued in the window goes to the server once, in order, and the server
answers each sent prompt with a placeholder reply, "(received)". Every window connected to the server shows every
tab's transcript and queue. A window that was away for long sees "N earlier entries not shown" in place of what it
missed (the server sends each transcript's newest 16 entries, so that a diff stays small however far behind a window
is). The friend list still comes from `nova-config` on the window's machine.

`nexus-server` keeps everything in memory: a restart loses it. It listens on a Unix socket only its user can use,
`~/.nexus/server.sock` (mode 0600, in a directory of mode 0700), logs to `~/.nexus/server.log` (never a key), and
takes UDP on ports 60001 to 60999, one per window. `nexus-server stop` stops it.

To try it, with `studio` a machine you can `ssh studio` to without a password prompt, and its UDP ports above
reachable from yours (over the tailnet they are):

```sh
# on studio: build nexus-server and put it on the PATH a login shell gives
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(sysctl -n hw.ncpu)" --target nexus-server
mkdir -p ~/.local/bin && install -m 755 build/nexus-server ~/.local/bin/nexus-server

# on your machine: build nexus and open it on the server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(sysctl -n hw.ncpu)"
open -n build/nexus.app --args --server studio
```

Not yet: the server's state lasting across a restart (the store layer); a window reattaching to a restarted server
(today it says it has had no contact); the friends' AIs answering (layer 3); the plus button's upload; iOS.

## What it builds on

- [Prax](https://github.com/pdoane/prax), the UI library, pinned.
- [mosh](https://github.com/mobile-shell/mosh) 1.4.0, the transport between the window and `nexus-server`: its
  network and crypto sources, fetched at the release tag and compiled unmodified (`cmake/mosh.cmake`). mosh is GPL v3
  or later; section 13 of the GPL v3 and of nexus's AGPL v3 allows the two to be combined, each keeping its own
  licence and notices.
- [nova-tools](https://github.com/mas-bandwidth/nova-tools) and
  [nova-sprint](https://github.com/mas-bandwidth/nova-sprint): nexus calls their command-line tools. It runs
  no AI models itself.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
open build/nexus.app
```

The window and `nexus-server` need protobuf for mosh's transport (on macOS, `brew install protobuf`; zlib and the
crypto come with macOS). Without it, configure with `-DNEXUS_LINK=OFF` for the window alone, without `--server`. A
Homebrew protobuf is built for the macOS it was installed on, and so then are nexus and `nexus-server` (the build says
so).

Tests: `ctest --test-dir build -L unit`, `-L functional`, and `-L model` (needs Java and `TLA2TOOLS_JAR`; see
[models/README.md](models/README.md)). The link's functional tests run `nexus-server` over `ssh localhost`, which has
to work without a password prompt; configure with `-DNEXUS_E2E_TAILNET=<this machine's tailnet address>` to run the
round trip over the tailnet too.

License: GNU AGPL v3.
