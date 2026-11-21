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

On macOS the client starts hidden at login and connects to its server. Command plus the tilde key
(the physical key left of 1, without Shift) toggles the overlay from any app. To set up login launch for a
client installed in `~/Applications/Nexus.app`, copy the [client LaunchAgent](macos/com.robotgamedevelopers.nexus-client.plist)
to `~/Library/LaunchAgents/`. It takes effect at the next GUI login. Its last argument is the server host,
which defaults to `localhost`.

The overlay is a see-through window over the whole screen, above other apps and their fullscreen
Spaces, with no Dock icon.
Escape hides it too, and so does switching away: Command-Tab, a click on another app, or Mission Control. With
`--windowed` it is a normal window instead.

Each friend the server holds (its `friends.json`, `docs/accounts.md`) has a tab, and a friend added or removed there
gets or loses its tab as soon as the server applies the change (`nexus-server config reload`); the sprint tab comes
first. Cmd-1 to Cmd-0 select the first ten. Drag a tab to
reorder the tabs; the order is kept across launches. Enter sends the prompt, Command-Enter queues it, and
Shift-Enter starts a new line. Cmd-Escape is stop. Cmd-plus and Cmd-minus make all text larger and smaller,
and the size is kept across launches.

Cmd-T opens a terminal tab, the login shell in the home directory, after the friends' tabs; Cmd-W closes it and goes back to
the tab selected before it, and it closes by itself when the shell exits. Terminal tabs can be dragged among themselves, never in among the friends. The
terminal tabs open at quit come back at the next launch, in the same order, each a new login shell in the directory
its shell was last in (the home directory if that is gone); what a terminal showed, and any program running in it, do
not come back. On a terminal tab every key that is not a Command chord, Escape included, goes to the shell. The
window opens on the tab that was selected when it quit, a terminal tab included.

On Windows a terminal tab is a WSL2 shell, never cmd.exe or PowerShell: the default WSL2 distribution's login bash in
its home directory (`wsl.exe --cd ~ --exec /usr/bin/env TERM=xterm-256color /bin/bash --login -i`), with the prompt its
own `.bashrc` sets. A restored tab starts in the directory its shell was last in when that is a Linux directory still
there, and in the WSL2 home otherwise. Without WSL2, or with no Linux distribution it can start, the tab shows one line
saying what is missing and how to install it (`wsl --install` in PowerShell); close it with Cmd-W and open a new one
once WSL2 runs. `wsl --status` in PowerShell says what WSL2 lacks, and `wsl --list --verbose` which distribution is the
default.

A terminal tab shows the title its program sets. While its shell runs ssh, the tab shows where ssh goes instead: the
fleet machine's name from `nova-config machine list` when the destination is the machine's name,
`<name>.<tailnet>.ts.net`, its address or any of those after `user@`, and otherwise the host as typed. A program on the
remote side that sets its own title does not change it. When ssh ends, the tab has its label from before the session
back until its shell sets a new title. A nested ssh on the remote machine is not seen: the tab names the machine the
local ssh reaches.

For those machine names and the sprint board's address, nexus finds `nova-config` on the PATH your login shell gives,
or at the full path its config names (`nova-config <path>` in `~/.nexus/client/config`, below); without it, ssh tabs
show the host as typed.

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
    kept-drafts.txt, kept-drafts.lock
                                the drafts of friends taken off a server, kept by server and name until they return,
                                and the lock every window's change of them takes
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
runs the configured friend’s driver. Its reply appears in the same tab. Every window connected to the server shows every
tab's transcript and queue. A window that was away for long sees "N earlier entries not shown" in place of what it
missed (the server sends each transcript's newest 16 entries, so that a diff stays small however far behind a window
is). The friend tabs are the server's friends, sent in its state; a server from before that list sends none, and the
window then shows the friends of the server's sessions.

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
nothing else: the token in `~/.nexus/server/accounts/<name>/oauth-token` reaches Claude on a descriptor, never in an
environment, and Claude keeps its own state in `~/.nexus/server/harness/claude/<name>` as `CLAUDE_CONFIG_DIR`. Any
`CLAUDE_CONFIG_DIR` or `CLAUDE_CODE_OAUTH_TOKEN` the server's environment carries is removed; the Keychain's login and
`~/.claude` are never used. Keeping the token from the AI it runs is best effort, not isolation: its shell runs in the OS
sandbox with the accounts' directory out of reach, its file tools are kept out of it, its network goes through a proxy
that refuses this machine's own addresses, and writing the files on its list of what runs outside the sandbox is
denied to it, by rule (a list, not complete). None of the routes the tests try reads the token; some routes stay open, listed per OS in [docs/secrets.md](docs/secrets.md) ("The token
wall"), among them this machine reached by way of another one. On Linux and WSL2 that sandbox's bubblewrap and socat
are built by nexus (`cmake/sandbox.cmake`), not installed. Without the directory, or with a token file that is missing,
not one line, or readable by anyone but its owner,
the driver refuses to start with a `no-credential` error naming the file. With the server running and the window connected, start the in-app sign-in for the configured account:

```sh
nexus-server config sign-in main
```

Replace `main` with the account linked to the friend. Follow the sign-in in Nexus, select the friend’s tab, type
a message and press Enter. The server reports the result of sign-in to the command.
See [accounts and friends](docs/accounts.md) for configuration.

Edit `policy/rules/client.star`, `server.star` and their JSON config in your Nexus fork. Both hosts run their own
rules. Preview a change with `build/nexus-policy preview`, then use `apply` to pick it up at the next decision
without rebuilding or restarting. The window application defines shortcuts, panels and tab registrations. The server application creates sessions, handles messages and selects accounts after a usage limit. See [wiring plugins in a fork](docs/plugins.md) for the native and Starlark registration APIs.
The driver launcher can load each friend's live nova-config definition, opening prompt and home documents. The Claude driver applies them on fresh and resumed sessions. See [customizing Nexus in your fork](docs/policy.md).

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

The Claude driver supports subscription conversations. Other providers, the upload button and iOS remain on the roadmap.

## What it builds on

- [Prax](https://github.com/pdoane/prax), the UI library, pinned.
- [mosh](https://github.com/mobile-shell/mosh) 1.4.0, the transport between the window and `nexus-server`: its
  network and crypto sources, fetched at the release tag and compiled unmodified (`cmake/mosh.cmake`). mosh is GPL v3
  or later; section 13 of the GPL v3 and of nexus's AGPL v3 allows the two to be combined, each keeping its own
  licence and notices.
- On Linux and WSL2, the Claude driver's sandbox programs, built from their release sources by `cmake/sandbox.cmake`,
  pinned by SHA-256 and compiled unmodified: [bubblewrap](https://github.com/containers/bubblewrap) 0.13.1 (LGPL v2
  or later), [socat](http://www.dest-unreach.org/socat/) 1.8.1.3 (GPL v2), and
  [libcap](https://git.kernel.org/pub/scm/libs/libcap/libcap.git) 2.78 (BSD or GPL v2, linked into bubblewrap). Each
  is a separate program nexus runs, keeping its own licence and notices.
- [nova-tools](https://github.com/mas-bandwidth/nova-tools) and
  [nova-sprint](https://github.com/mas-bandwidth/nova-sprint): nexus calls their command-line tools. It runs
  no AI models itself.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
open build/nexus.app
```

The window and server builds use Rust and Cargo (with the dependency versions locked in `policy/native/Cargo.lock`)
to embed Starlark and build the policy CLI.

The window and `nexus-server` need protobuf for mosh's transport (on macOS, `brew install protobuf`; zlib and the
crypto come with macOS). Without it, configure with `-DNEXUS_LINK=OFF` for the window alone, without `--server`. A
Homebrew protobuf is built for the macOS it was installed on, and so then are nexus and `nexus-server` (the build says
so).

Tests: `ctest --test-dir build -L unit`, `-L functional`, and `-L model` (needs Java and `TLA2TOOLS_JAR`; see
[models/README.md](models/README.md)). The link's functional tests run `nexus-server` over `ssh localhost`, which has
to work without a password prompt; configure with `-DNEXUS_E2E_TAILNET=<this machine's tailnet address>` to run the
round trip over the tailnet too.

License: GNU AGPL v3.
