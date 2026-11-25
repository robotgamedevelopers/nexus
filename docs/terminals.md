# Terminals on the server

nexus-server runs terminals: a login shell on a pseudoterminal of the server's own, whose screen the server keeps. A
window creates one, types into it, resizes it and closes it through its numbered actions over the link it already has,
and sees it through the state the server sends. A terminal keeps running while no window is linked; a window that links
again finds it as it left it.

This is how the iPhone and the iPad have terminal tabs: an iOS app cannot start a process, so its terminal tab is a view
of a shell that runs on the server's machine. The Mac and Windows windows keep their own local terminals (see "The
desktop windows" below).

## The pieces

| Piece | Where | What it does |
| --- | --- | --- |
| The book | `src/server/terminal_book.h` | Decides each terminal action: who may, which terminal it names, what the host must do. Pure, no I/O. |
| The host | `src/server/terminal_host.h` | Does what the book decides: starts each shell (`forkpty`), writes input, resizes, ends; reads each shell's output into its own screen (Prax's `lib/vterm`), and gives the screens to the state. |
| The modes | `src/server/terminal_screen.h` | The input modes a program sets (cursor keys, keypad, mouse and focus reports, bracketed paste), read from the program's output, so that a window's keys follow them. And each screen row as printable text with SGR runs. |
| The state | `src/sync/terminal_state.h` | `TerminalView`, a terminal as the state carries it; `terminalFate`, what a window's tab makes of the state; the paint of a view onto a window's own screen (`sync/terminal_paint.h`). |
| The wire | `schema/Nexus.schema` | `SessionsDiff.terminals_served`, `terminal_epoch`, `terminals`; the action kinds `TerminalCreate`, `TerminalInput`, `TerminalResize`, `TerminalClose`; `Action.terminal`, `columns`, `rows` (`docs/formats.md`). |
| The iOS tab | `src/remote_terminals.h` | A terminal tab whose shell is the server's: Prax's terminal view with no program behind it, painted from the state, its keys sent as input. |
| The model | `models/ServerTerminals.tla` | The lifecycle, its invariants and liveness, reversed witnesses, and a trace check of the real code (below). |

## The shell

A terminal's shell is the one the Mac window's terminal tab runs: the person's login shell (`SHELL`, else the password
database's, else `/bin/sh`) with `-l`, in the directory the create names when it is one, else the home directory. Its
environment is the server's, less every `NEXUS_` variable (the server's own settings, which no shell needs), with
`TERM=xterm-256color` and `COLORTERM=truecolor`. It runs as the server's user: the server's owner.

The server runs at most 16 terminals (`kMaxTerminals`); a create past that is refused, and the window's tab says why. A
screen is at most 500 columns by 300 rows; a create or resize past that is cut to it. One action carries at most 64 KiB
of input; a window sends more as several, in order.

## Lifecycle

A terminal is **running** from its create until its shell ends, then **ended** (its last line, and the state's `ended`,
say how: `exited with code N` or `ended by signal N`) until a window closes it. A close of a running terminal sends its
shell's process group SIGHUP and the server reaps it. A server that stops ends every shell (SIGHUP, then SIGKILL to what
is left 200 ms later).

Each run of the server has a random nonzero **epoch**, and a terminal's id holds it in its high 32 bits: an action
naming a terminal of another run is refused (`ended when the server restarted`), so input typed for a terminal lost to
a restart never reaches a new one in its place. The state carries the epoch, and a window whose tab held a terminal of
another epoch shows `This terminal ended when nexus-server restarted.` until the tab is closed.

A window's actions are applied once each, in number order, by the store's durable counter (`sync/actions.h` Applier),
across the window unlinking and the server's restart. A window unlinking changes no terminal.

What a window's tab does with the state (`sync::terminalFate`):

| The state | The tab |
| --- | --- |
| holds the terminal, running | shows its screen and sends what is typed |
| holds the terminal, ended | shows its screen, with the line that says how; takes no input; stays until closed |
| does not hold it, same epoch | another window closed it: the tab goes |
| does not hold it, another epoch | shows that the server restarted; stays until closed |
| holds a terminal no tab holds | a tab of its own at the end of the terminals; the selection stays |

## The model

`models/ServerTerminals.tla` models the server's terminals and the windows that use them: create, input, resize and
close as each window's numbered actions; the server applying each once, in order, by its durable counter; a window
linking and unlinking; a shell exiting; the server restarting with a new epoch; and each window taking the state into
its tabs. Its properties:

- `InputOnceInOrder`: each window's keys reach a shell once each, in the order typed.
- `InputOnlyToItsRun`: a shell is given only input typed for its own run of the server.
- `NoInputUnlessRunning`: input reaches a terminal only while its shell runs: never an ended, closed or lost one.
- `DetachKeepsRunning`: a window unlinking changes no terminal.
- `RestartShown`: a tab's terminal leaves the tab only when it was closed in the run the tab knows.
- `ExitShown` (liveness): a shell's end reaches every window that stays linked.
- `RestartNeverSilent` (liveness): a terminal lost to a restart is shown lost to a window that links again.
- `ActionsApplied` (liveness): every action of a linked window is applied.

Each property has a reversed witness in `models/witnesses/WTerm*.tla`, a broken model TLC must reject: an Applier that
forgets the store's counter (`WTermReplay`), ids without the epoch (`WTermAnyEpoch`), input to an ended shell
(`WTermInputAfterEnd`), a shell tied to its window's link (`WTermDetachKills`), a tab closed whatever the epoch
(`WTermRestartSilent`), an ended terminal shown running (`WTermExitHidden`), a lost terminal kept as it was
(`WTermRestartKept`), and an Applier that stalls (`WTermApplyStalls`).

The model is bound to the code: `tests/model/terminals_walk.cpp` drives the real `TerminalBook` (one per run of the
server, each with its epoch), `sync::Applier` (made on every link from the store's counter, given every action not yet
acknowledged) and `sync::terminalFate` with a seeded random walk, and `models/ServerTerminalsTrace.tla` checks that each
trace is a behaviour of the model. Three faulty walks must be rejected: an Applier made from 0 (`--faulty-replay`), a
fate asked without the tab's epoch (`--faulty-fate`), and every run of the server given the same epoch
(`--faulty-epoch`). The checks: `ctest -L model -R tlc_terminals` (TLC; the nightly's `terminals` shard), and the walks
under the sanitizers in CI.

## What the server keeps, and where

- **The screens and the input modes:** in the server's memory only, in each terminal's emulator. Never in the store,
  a state file (`nexus-server` writes a state file without them), a log or any other file.
- **Input:** in the action's secret (`sync::SecretValue`), which is wiped when it is freed; on the server, input the
  pseudoterminal cannot take yet waits in memory and is zeroed as it is written.
- **The store:** each terminal action, as any action, advances the window's durable counter (one journal record),
  which says only that the action with that number was applied. Its kind, terminal and content are not written.
- **Logs:** a refused terminal action is logged by its kind and terminal number (`terminal-input for terminal N`),
  never its input or a screen.
- **Files the shell touches** are the shell's, as in any terminal.

The server's files are where they were: the store and its socket in the server's directory (`0700`, the socket
`0600`). Terminals add no file, no port and no key.

## Who gets a shell

A terminal is a shell as the server's user. The server gives one only to a link made by its owner:

- A window's link is made by a request on the server's control socket (`server.sock`, `0600`, in the server's
  directory, `0700`), and the server reads the asking process's user from the socket itself (the peer's credentials:
  `getpeereid` on macOS, `SO_PEERCRED` on Linux). It answers no other user's request at all; and a link it made for
  its own user is marked the owner's (`Link.owner`), and a terminal action from any link not so marked is refused
  (`a terminal runs a shell as the server's owner, and this link was not made by its owner`) and counted: two checks,
  so that a later way of making links cannot hand out shells by accident.
- The windows reach the control socket through ssh as that user (`nexus-server new`, `docs/ios.md`): so the link is
  already authenticated by ssh, by the person's key. Terminals add no new port, no new key and no new way in.
- The iPhone's and iPad's key runs only `nexus-server new` and `nexus-server history-page` on the server's machine
  (`tools/nexus-ios-gate`, unchanged): no shell, no forwarding. Through the link that command makes, it can now open
  terminals; this is the design, and the gate is not widened for it.
- The AIs nexus runs cannot reach the control socket: a limited AI's sandbox denies `~/.nexus` and unix sockets
  (`drivers/claude/src/wall.ts`), so no AI opens a terminal through nexus.
- A terminal's pseudoterminal is closed on exec in the server, so the AIs' drivers and other children never inherit
  another terminal's master.

## Threats

| Who | What they could try | The answer |
| --- | --- | --- |
| Someone holding the **unlocked** phone or iPad | Open a terminal, or one already open, and type into the server's shell | The whole app goes behind one biometric lock (the app lock, built for the iOS port on its own branch; terminals add no lock of their own and rely on it): a terminal is a tab like any other behind it. Past the lock, the person holding the device is the owner as far as the server can tell: whoever can use the app can use a shell, as with an unlocked laptop with a terminal open. Until the app lock lands, the device's own lock is the only one. |
| Someone holding a **stolen, locked** phone | Read a terminal's screen, or the input sent | The app holds the screens in memory only: nothing in its files, logs or the store, and the input is wiped once the server has it. Hiding the window from the system's app-switcher snapshot is the app lock's. The ssh key is a file in the app's container, under the system's data protection; remove its line from `~/.ssh/authorized_keys` on the server to cut the device off. |
| The **network** | Read or change what is typed or shown | The link is ssh to start (the person's key) and then mosh's encrypted, authenticated channel with a key ssh carried: nothing a terminal sends crosses the network in the clear, and nothing injected is accepted. |
| **Another app** on the phone | Open a terminal, or read one | No URL scheme, no extension, no intent opens a terminal: only a tap in the app's own window does. The screens are never put on the pasteboard except by the person copying a selection. |
| **Another user** on the server's machine | Open a terminal as the owner | The control socket is the owner's (`0600` in `0700`), and the server checks the peer's user on every link: another user's link gets no shell. |
| An **AI** nexus runs | Open a terminal through nexus to escape its sandbox | Its sandbox reaches neither `~/.nexus` nor unix sockets; and a link that is not the owner's gets no shell. |
| An old **phone** key that should only talk | Get a shell | Today any key that may run `nexus-server new` may open terminals. A per-key option that refuses terminals is on the roadmap (`server-terminal-key-option`). |

## The desktop windows

The Mac and Windows windows keep their local terminals: a terminal tab there is a shell the window starts on its own
machine, as before. To move them to server terminals:

1. Command-T asks the server (`TerminalCreate`) when the window is linked to a server that runs them, through
   `remote_terminals.h` in place of `terminals.h` (the seam is the same: `view.h` `Terminals`); a window with no server,
   or one older than terminals, keeps starting local shells.
2. The tab's label: the server's terminal has the title its program set; the desktop's label also names the machine an
   ssh in the tab reaches (`remote.h`), which the server would read off its own foreground process and send.
3. Scrollback: the desktop's terminal keeps 10,000 lines; the server would keep a bounded scrollback and a window ask for
   what it scrolls to, as it asks for a transcript's history (`server-terminal-scrollback`).
4. The pointer: mouse reports to the program (`server-terminal-pointer`).
5. The local niceties the desktop's terminal has from Prax (shell integration's prompt marks, images, notifications)
   would come from the server's emulator, or stay local-only.

Windows' WSL2 terminal and a window with no server keep the local terminal in every case.

## What a window does not get yet

- Scrollback: the window shows the screen; what scrolled off is the server's emulator's, which keeps none.
- Mouse reports to the program: the pointer selects and scrolls in the window's copy.
- Images and notifications from the program.
