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
Shift-Enter starts a new line. Cmd-Escape is stop. Cmd-plus and Cmd-minus make all text larger and smaller,
and the size is kept across launches.

Cmd-T opens a terminal tab, the login shell in the home directory, after the friends' tabs; Cmd-W closes it and goes back to
the tab selected before it, and it closes by itself when the shell exits. Terminal tabs can be dragged among themselves, never in among the friends. The
terminal tabs open at quit come back at the next launch, in the same order, each a new login shell in the directory
its shell was last in (the home directory if that is gone); what a terminal showed, and any program running in it, do
not come back. On a terminal tab every key that is not a Command chord, Escape included, goes to the shell. The
window opens on the tab that was selected when it quit, a terminal tab included.

A terminal tab shows the title its program sets. While its shell runs ssh, the tab shows where ssh goes instead: the
fleet machine's name from `nova-config machine list` when the destination is the machine's name,
`<name>.<tailnet>.ts.net`, its address or any of those after `user@`, and otherwise the host as typed. A program on the
remote side that sets its own title does not change it. When ssh ends, the tab has its label from before the session
back until its shell sets a new title. A nested ssh on the remote machine is not seen: the tab names the machine the
local ssh reaches.

nexus finds `nova-config` on the PATH your login shell gives, or at the full path its config names (`nova-config
<path>` in `~/.nexus/client/config`, below).

The window writes its errors to stderr. On Windows it has no terminal, so unless stderr was redirected they go to
`%USERPROFILE%\.nexus\client\nexus.log`, appended to on each launch; if the hotkey does not register, a message box says
so and names that file. On Windows only the window is native: `nexus-server` and the link run as the Linux build inside WSL2, and
the Windows window has no `--server` yet.

## Where nexus keeps things

Everything nexus keeps lives under `~/.nexus` (mode 0700), in two homes, each mode 0700 and each with its own
config. Whichever side starts first makes `~/.nexus`; each side makes its own home. Every token and key is a file of mode 0600 there: none is read from
the Keychain, the environment or another tool's directory (`~/.claude` included).

```
~/.nexus/client/                the window
    config                      its settings: `nova-config <path>`, `sprint-url <url>`
    tab-order.txt, selected-tab.txt, text-scale.txt, terminals.txt
                                its saved order, selection, text size and terminals
    nexus.log                   Windows only: its errors
~/.nexus/server/                nexus-server (`--dir` gives another directory)
    config                      its settings (it takes none yet)
    drivers                     which program drives which friend
    store/                      the tabs and what each window has had applied
    server.sock, server.log     its control socket (mode 0600) and its log
    accounts/<name>/            one harness login each (always here, whatever `--dir` says)
        oauth-token             the Claude account's token, mode 0600
```

A config file holds one setting per line, `<key> <value>`; a line starting with `#` is a comment. A key the side does
not know is a warning and skipped; a key with no value, or one given twice, is an error naming the file and the line
(the server does not start on one; the window logs it and goes on without that line). No config file means the
defaults: `nova-config` found on the login shell's PATH, and the sprint board found from the sprint's own record.

On Windows the two sides are two machines as far as nexus is concerned: the native window's home is
`%USERPROFILE%\.nexus\client` (with its own config), and `nexus-server`, inside WSL2, keeps its own
`$HOME/.nexus/server` in the Linux home there. They are different directories on purpose; each side reads only its own.

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

`nexus-server` keeps every tab's transcript and queue, and how far it has applied each window's actions, in
`~/.nexus/server/store/`: each action is written and synced there before any window sees it, so a restart, or the server
killed, loses nothing a window was shown. A window that names itself (`nexus-server new --client <id> --next <n>`)
can open a new session after a restart and send its actions again from action n under the same numbers; the server
applies only those it had not, and refuses a session that would skip some. It listens on a Unix socket only its user
can use, `~/.nexus/server/server.sock` (mode 0600, in a directory of mode 0700), logs to `~/.nexus/server/server.log`
(never a key),
and takes UDP on ports 60001 to 60999, one per window. `nexus-server stop` stops it. If the disk fails under the
store, the server stops; started again, it opens the store from what is durable.

A friend's AI is reached through a driver, a child process of `nexus-server` per friend that speaks the line protocol
in [docs/driver-protocol.md](docs/driver-protocol.md). Which program drives which friend comes from `~/.nexus/server/drivers`
(an example: [drivers/claude/drivers.example](drivers/claude/drivers.example)). The Claude driver,
[drivers/claude](drivers/claude), runs Claude on the Claude Agent SDK (TypeScript, Node 20.19 or newer; the SDK is a
dependency fetched by npm, never kept in this repository) with permission prompts bypassed, in the friend's directory.
It runs only on a subscription: it removes every API-key variable from Claude's environment, loads no Claude settings
files unless told to, and refuses any session that would bill an API key, another provider or paid extra usage, with
a `no-credential` error saying why and what to do. Its login is the account its `--account <name>` names, and
nothing else: Claude runs with `~/.nexus/server/accounts/<name>` as `CLAUDE_CONFIG_DIR` and the token in its
`oauth-token` file as `CLAUDE_CODE_OAUTH_TOKEN`, in Claude's environment only. Any `CLAUDE_CONFIG_DIR` or
`CLAUDE_CODE_OAUTH_TOKEN` the server's environment carries is removed; the Keychain's login and `~/.claude` are never
used. Without the directory, or with a token file that is missing, not one line, or readable by anyone but its owner,
the driver refuses to start with a `no-credential` error naming the file. To log an account in:

```sh
mkdir -p -m 700 ~/.nexus/server/accounts/main
CLAUDE_CONFIG_DIR=~/.nexus/server/accounts/main claude setup-token   # sign in with the subscription account
# save the token it prints, alone on one line, with mode 0600:
(umask 077 && cat > ~/.nexus/server/accounts/main/oauth-token)       # paste, Enter, then Control-D
```

Model and effort can be set in the drivers file and
changed between turns. Build and test it:

```sh
cd drivers/claude
npm ci            # the pinned SDK and tools, from package-lock.json
npm run check     # compile (strict), lint, unit tests
NEXUS_LIVE_ACCOUNT=<name> npm run test:live # opt-in: the real driver on the real SDK, on ~/.nexus/server/accounts/<name>; spends the subscription
```

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

Not yet: the window reattaching to a restarted server by itself (today it says it has had no contact; the server
side of it is there); the friends' AIs answering (layer 3); the plus button's upload; iOS.

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
