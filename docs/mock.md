# The mock: a server of fake AIs

`nexus-mock` starts a real `nexus-server`, with its real store and its real link, whose AIs are fakes: each AI's driver
is the fake driver (`tests/driver/fake_driver.cpp`) playing a script over the [driver protocol](driver-protocol.md).
Every client reaches it the way it reaches any server, so the mock drives the Mac window, iPad, iPhone, Apple Watch and
Windows clients for development, screenshots and tests without touching anyone's real AIs, accounts or sign-ins.

No fake AI has an account, so none needs a sign-in: each is ready to talk the moment a client connects. Nothing outside
the mock's directory is read or written: the server's HOME is the mock's own, no harness or credential is involved,
and the only network is the link on this machine.

## Start it

The mock is built with the link (`cmake --build build`; it is `build/tests/mock/nexus-mock`):

```
build/tests/mock/nexus-mock start
```

It makes the mock afresh in `/tmp/nexus-mock` (`--dir DIR` for another; an earlier mock there is stopped and removed
first, and a directory that is not a mock's is refused), starts its server, plays each AI's seeded conversation through
the real link, and prints the command a client runs to reach it, which is also in `<dir>/server-command`:

```
/usr/bin/env HOME=/tmp/nexus-mock/home <build>/nexus-server new --dir /tmp/nexus-mock/server
```

`nexus-mock command` prints it again; `nexus-mock stop` stops the server and removes the directory.

| option | what it gives |
|---|---|
| `--ais N` | N AIs (default 5: Ada, Grace, Linus, Margaret, Alan). The presets are `1`, `5`, `12` and `30`, for menus and scrolling; `0` is the state with no AIs. A server holds at most 16 sessions (`sync::Limits`), so past 16 the AIs are listed, with tabs, and only the first 16 have sessions and seeded conversations, as with any server |
| `--names A,B,...` | these AIs, in this order |
| `--sign-in` | one more AI, Ida, whose account has no login, to show the sign-in screen. Off by default. A sign-in started there runs a FAKE `claude` (`tests/server/fake_claude.cpp`) that hands its made-up URL to nobody: it never reaches a real login, page or network |
| `--instant` | replies to new prompts written at once, for tests; without it they stream a word at a time at a reading pace, after a pause to think |

## Point a client at it

A client runs the mock's command on the server's machine in place of `nexus-server new`, over ssh as always (to
`localhost` when the client is on the same machine).

- **Mac window.** `--server-command`, with a HOME of the mock's own so that the window shares nothing with yours (its
  tab order, drafts and lock are under `<dir>/client`):

  ```
  HOME=/tmp/nexus-mock/client build/nexus.app/Contents/MacOS/nexus --server localhost --server-command "$(build/tests/mock/nexus-mock command)"
  ```

  The client config's `server-command <words>` does the same as the flag.
- **iPad and iPhone** (the `port/ios` and `ios/iphone-form` branches, not yet on `main`): the client config's
  `server user@host` and `server-command <words>` in the app's container, once the app reads `server-command` as the
  window does. Its ssh key runs behind `tools/nexus-ios-gate`, which has to let the mock's command through too.
- **Apple Watch** (the `watch/client` branch): the phone's relay takes the command as `nr_host_new`'s `command`
  (`watch/host/nexus_relay.h`), as the Mac's relay host in its simulator test already does with a server of its own.
- **Windows** (the `port/windows` branch): the relay in WSL2 runs the window's link with `--server HOST`; it needs to
  pass `--server-command` on to it.

A screenshot needs neither ssh nor a display: `nexus-mock state --out FILE` connects as a client and writes the state
the server sends, and the window draws it as it shows it (`--tab N` picks the tab; the dashboards come first, then the AIs):

```
build/tests/mock/nexus-mock state --out /tmp/nexus-mock/state
build/nexus.app/Contents/MacOS/nexus --screenshot ada.png --state /tmp/nexus-mock/state --tab 1
```

Only the Mac window's way is on `main` today. Each other client reaches the server through the same bootstrap
(`link::Bootstrap::command`, read with `link::serverCommandWords` in `src/link/bootstrap.h`), so each needs only to take
the words and hand them on.

## What the AIs say

Each AI has a seeded conversation, played through the server before any client connects: a short reply, a long reply
that wraps, markdown with headings, lists and a quote, a code block with long lines, a turn with tool lines, a turn
stopped part way (the mock sends the stop), a turn that ends in an error, a turn cut short by the driver's exit, and a
last short reply, so that every AI is idle with its driver running when a client connects. A new prompt gets the next
of three scripted replies, round and round: a short answer, a list after a tool line, and code. The text is in
`tests/mock/mock.cpp`; each AI's script is in `<dir>/scripts/<name>.script`, and the fake driver's steps are at the top
of `tests/driver/fake_driver.cpp` (`prompt`, `stream`, `pause` and `option cycle` were added for the mock).

## What is in the directory

| path | what |
|---|---|
| `nexus-mock` | the marker that says the directory is a mock's: start and stop remove only such a directory |
| `home/` | the server's HOME |
| `server/` | the server's `--dir`: `drivers`, `config`, `nexus-config/`, `store/`, `server.sock`, `server.log` |
| `scripts/`, `work/` | each AI's script, and the directory it works in |
| `bin/claude-silent` | the FAKE `claude` the sign-in preset's sign-in runs |
| `client/` | a HOME for a window run against the mock |
| `server-command` | the command a client runs |

## Tests

`mock_unit` checks what the mock writes; `mock_e2e` (functional) starts `nexus-mock` for real and connects through the
link: the AIs and their seeded transcripts arrive, a prompt gets its scripted reply exactly once, only the sign-in
preset's AI needs a sign-in, and the state with no AIs is an empty list. `driver_fake_stream` and
`driver_fake_prompts` check the fake driver's new steps. The mock adds no state machine: its turns are the fake
driver's, which keeps the protocol's sequence rules (a stop mid-stream ends the turn as rule 7 says), and the server,
store and link are the real ones with their own models.
