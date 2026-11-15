# The secret dialog

How a secret (an API key, a token) gets from a person's fingers to its file on `nexus-server`, with no AI ever
seeing it. The owner's rulings (2026-11-14): everything about logging in and staying logged in is built into the app;
a service that needs an API key gets one paste, in the app, and it just works; the client does the sign-in and sends
the server what it needs; the same holds for any secret nexus asks for; a little dialog pops up, nobody sees the
secret, it goes to the server, and it is handled there. With AGENTS.md: a secret lives under
`~/.nexus/server/accounts/<name>/` (0700 directories, 0600 files) and is never shown, logged, or seen by an AI.

The state machine is `models/Secrets.tla` (`models/README.md` has its properties, instances and witnesses). This page
is the design the code is built to. The code is bound to the model by a trace check in CI; "Binding and leak tests"
below says what the binding covers, and what it does not yet.

## The flow

1. An **asker** asks `nexus-server` for a secret for an account: a friend's AI, through its driver (below), or the
   server itself (the sign-in flow, roadmap 5c).
2. The server refuses the ask outright (below: no request, no id) unless the account and what is asked are each a name
   (letters, digits, `-`, `_` and `.`, at most 64), the account exists, the asker's friend is linked to it in
   `friends.json` (an AI asks only for the accounts its friend may use; nexus itself may ask for any), the account's
   provider documents the file, and the reason is one line of plain text. It refuses at once, answering the asker
   `refused` (`busy`), when a request is already open for that account, when the same asker has one of its own open
   already, or when it has asked 5 times in the last minute (a guess, not a measurement: enough for a person to fix
   a few logins in a row, few enough that an AI cannot fill the dialog); nexus's own asks are never rate-limited. It
   refuses `no-client` when no connected window can take a request (none connected, or only windows from before
   secret requests). Otherwise it opens a **request**: an id it mints, the account, what is asked (the file's name,
   such as `api-key`), and who asks.
3. Every connected window that can take requests receives it in its state. The server knows no tabs (AGENTS.md), so
   it offers the request to every such window, not to the one showing the asker's session.
4. A window shows a dialog for the oldest open request it has not answered, one at a time. A person pastes the value
   into one window and presses Enter, or presses Escape to cancel.
5. Enter sends the value up that window's actions to the server, once, and closes that window's dialog. Escape sends
   a cancel. Nothing is ever sent to another window.
6. The server takes the first answer for an open request, against the request's state when it processes it. A value
   not of a token's shape (nothing, more than 16 KiB, or anything but printable ASCII without spaces: two words, a
   line break inside it, a pasted sentence) is refused `malformed`, saying why and never what. Otherwise it writes the
   account's file and answers the asker `stored` once the file is durable; a file it cannot write yet does not cost the
   paste (below, "One chance"). A cancel answers the asker `refused` (`cancelled`). A request nobody answers in time
   ends `refused` (`timeout`).
7. The request leaves the state, and every other window closes its dialog when its state no longer holds it.
8. An answer for a request that is no longer open (answered in another window, cancelled, or from before a restart) is
   dropped: nothing is stored, and the log records the request id and `late`, never the value. One answer is not
   dropped: one that crossed its request's deadline on the way (the person pasted it as the deadline passed) is stored
   while its request is still the newest of its account, since the person pasted it for exactly that file.

## One chance: a pasted value is never lost

The owner's rule (2026-11-14): a person pastes a key or signs in once, ever. nexus never loses what was pasted and
never asks for it again; asking again is a bug. The model's invariant is `ReceivedNeverDropped` (`models/Secrets.tla`):
while the server runs, a value it received is stored, or held and being written again.

- **Held, not refused.** When the account's file cannot be written (a full disk, a directory the server refuses as it
  lies, an I/O error), or the write lands but the directory's sync fails so its durability is not confirmed, the
  server keeps the value in its memory (a `SecretValue`, wiped when it goes) and writes it again: 1 s later, then 2,
  4, and so on, at most 60 s apart, until the write is durable. The asker hears nothing until then; its session says
  once that the value arrived and is held, and `server.log` says why at each try, with the filesystem's error and
  never the value. No further request opens for the account while a value is held. When it is written, the asker
  hears `stored` (or, if its turn is gone, the session says so) and the account signs in if the file is its login.
- **`stored` only when durable.** The asker never hears `stored` for a file that a crash could still take back.
- **The window.** The held value lives only in the server's memory: if the server ends before the write succeeds
  (stopped, crashed, the machine restarted), the value is gone, and `server.log` says so (`lost`). Then, and only
  then, the account is asked again. The window is as small as the cause allows: the first write happens in the same
  pass the answer arrives in, before the window is told the action arrived, so a server that is up with a writable
  disk has nothing held at all; a value is held only while the disk refuses it, for as long as it refuses.

Every way an ask ends, by whose it is:

| outcome | whose | what happens |
|---|---|---|
| `busy` | neither: no value exists yet | refused at the ask, before any dialog; nothing was pasted |
| `no-client` | neither: no value exists yet | refused at the ask, no window can take it; nothing was pasted |
| `cancelled` | the person's | they pressed Escape: nothing was pasted |
| `timeout` | the person's | nobody pasted in 10 minutes. An answer that crossed the deadline is stored (step 8) |
| `malformed` | the person's input | told why (never what); asked again: nexus's own asks at once, a friend's AI by its own ask |
| file cannot be written | ours | held and written again until durable; never refused (the old `unwritable`) |
| durability not confirmed | ours | held and written again until durable (the old `stored` with `unconfirmed`) |
| late: another window stored it | neither | already stored; nothing lost |
| late: after a cancel | the person's | they cancelled it |
| late: from before a restart | ours, the documented window | the request ended with the server; nexus asks again |
| held when the server ended | ours, the documented window | logged `lost`; nexus asks again |

A login the provider has revoked, or one that ended and cannot be renewed, is the one other case where a second paste
is asked for, and the dialog says which (`docs/accounts.md`, "How long a login lasts").

A server restart drops every open request without an answer: its asker is gone with it (a driver ends with the server,
`docs/driver-protocol.md`), and the server's own sign-in asks again. Request ids are never reused, across restarts
too: the store keeps the last one minted as the applied counter of a client named `nexus:secret-requests`, a name no
window can take, committed before the request opens; so a dialog left open across a restart can never satisfy a later
request. A window that loses its link or quits loses its dialog and anything typed in it; the request
stays open for the other windows, or until its deadline.

The asker hears exactly one answer per request: `stored`, or `refused` with one reason of `busy`, `no-client`,
`cancelled`, `timeout` or `malformed`. Never the value, never its length, never which window answered. Servers before
the one-chance rule also answered `unwritable` and `stored` with `unconfirmed`; this one never does (it holds the value
instead), and a driver still reads both.

`stored` goes out only once the file is in place: the value is written to a temporary 0600 file, synced, renamed over
the old one and the directory synced before the hub hears it was kept, and the asker's driver line is held, with
everything else the server's pass made, until the pass's flush (`server/pass.h`). A crash at any point leaves the old
value or the new one, never part of one (`secrets_test files_crash`). If the rename succeeded and only the directory's
sync failed, the value is held and written again like any failed write (above), since a crash before the disk settles
may bring the old file back; `server.log` gives the filesystem's error. A temporary file a crash or a failure left behind holds a value no one reads: the server removes
every `*.new` in every account's directory when it starts, before anything is asked, and logs each one it removed.

The account's directory is the server's alone. Before a write the server checks it with `lstat`: a directory that is a
symbolic link, is not a directory, belongs to another user, or is open to group or others (anything wider than 0700)
is not written to, with the fix in the log, and the value is held until the directory is fixed; the server never follows a link there and never widens or narrows
a directory it did not make. The temporary file is created new (`O_CREAT|O_EXCL|O_NOFOLLOW`, 0600), and the value is
written and synced through that one descriptor, never reopened by its path, so a link planted at its name stops the
write rather than redirecting it. When the stored file is the account's login
(`oauth-token` for a claude account), the account counts as signed in at once, and each session held for want of a
credential starts its driver for the prompt waiting, without a restart (`Supervisor.tla` `SignIn`).

## The dialog

- It belongs to the overlay, like any pop-up, on whatever tab shows. It names the account, what is asked and who asks
  (a friend and session, or nexus itself), with the asker's one-line reason shown as quoted text: it is data, never an
  instruction to the person.
- nexus never shows itself for a request (the launch rule: only the hotkey shows the overlay). A request that arrives
  while the overlay is hidden waits for the next show.
- It is Prax's dialog (`prKitDialog`, the highest layer that does the job) holding one Prax text field of the password
  kind, with no placeholder and no hint. The field has the focus while the dialog shows, and while it shows no event
  reaches the sprint tab's page or a terminal's shell (`FrameHost::event`). One paste (Command-V) fills it; typing works
  too; a paste's line break and the spaces at its ends are cut at Enter. Enter sends; Escape, the close button or a
  press outside the card cancels the request, for every window (Escape backs out one context, and this dialog is one,
  on a terminal tab too).
- The mask is Prax's: one U+2022 for each code point, so the length shows. A fixed row of dots, whatever the length, is
  what this design wants; Prax has no such mask yet, and the request is on the roadmap (`prax-fixed-mask`).
- Nothing echoes the value: Prax keeps a password out of the clipboard (copy and cut do nothing), and the window's
  layout holds only the mask (the frame test checks that no node's text holds the value). It never enters the window's
  draft state, its saved preferences or a state file; a window drawn from a state file (`--state`, `--screenshot`)
  has no link and shows no dialog. After Enter or Escape the field is cleared, and nexus's own copies of the value (the
  prompt's, the action's) are overwritten. Two copies are not nexus's to overwrite: Prax's text field keeps the value
  in its own edit buffers until their memory is reused, and a value the person copied stays on
  the system clipboard, where nexus put nothing and can clear nothing. A request to Prax for a password field that
  wipes its buffers is filed (`prax-wipe-password-buffers` on the roadmap).
- No core file of nexus holds a value: the window and `nexus-server` set `RLIMIT_CORE` to 0 (soft and hard) as they
  start, and on Linux mark themselves not dumpable (`PR_SET_DUMPABLE` 0), which also keeps other processes of the same
  user from reading their memory through `ptrace` or `/proc` (`src/no_core.h`). A refusal is logged and the process
  goes on.
- Nothing in it waits: the requests come with the state the link's thread already holds, and Enter hands one action to
  that thread. The frame test (`docs/frame-budget.md`, `secret_dialog`) opens it, types, pastes, sends and cancels,
  every frame within 8.3 ms.
- When the request ends elsewhere (answered in another window, cancelled, past its deadline), the dialog closes. The
  outcome shows as one line in the asker's session (`stored`, or `refused` and why), with no value.

## The wire

The value travels in exactly one place: one action, from the window that took it to `nexus-server`, over the link's
mosh transport (encrypted with the session's key). Nothing else on the wire, in any direction, carries it.

- **Server to window.** The sessions state gains the open requests, after the sessions: each one framed by its
  length (`docs/formats.md` rule 1) and holding the request id, the account, what is asked, the asker (a session id,
  or 0 for nexus itself), the asker's friend, and the asker's one-line reason. Never a value. In format 3 the list is
  always there (a count, 0 for none), so a state cut short before it is refused rather than read as none. This is a
  new state format (3), agreed when the window connects as today; a window that agrees an older format receives no
  requests, and the server counts it as unable to take one.
- **Window to server.** Two new actions: a secret answer (the request id and the value) and a secret cancel (the
  request id). The actions are unframed today, and `docs/formats.md` says their first change comes with a framed
  format agreed the same way: the actions become framed with state format 3, and a window sends these two only for a
  request it received, which no older server ever sends. Should one ever be written in format 2, it is written as a stop
  naming session 0, which every server refuses and counts: the value never leaves the window.
- **The value in the window.** It is held in the action until mosh's transport has the server's acknowledgement, and
  then the subtract that drops acknowledged actions drops it; the window overwrites its own copy. Actions are never
  written to disk, and their debug description gives a secret answer's kind and request id only.
- **The value in the server.** The applier hands a secret answer to the request's handler, which writes
  `accounts/<name>/<what>`: the directory made 0700 if missing, the value written to a temporary 0600 file beside it,
  synced and renamed over the old one, and the directory synced (`src/server/secret_files.h`). It never goes into the
  store, a record, a snapshot, the state, a transcript, a
  driver's input or `server.log`; the log line for an answer is the request id, the account and the outcome. The
  request itself is not durable (it holds no secret, and it ends with the server). While a write fails, the value is
  held in the server's memory and nowhere else ("One chance", above).

## How an AI asks: the driver protocol

These are additions to `docs/driver-protocol.md`, version 1: each is a kind an older reader skips (rule 8), so no
version change. They land with the code, in the same pull request as `src/driver/protocol.h`.

| direction | kind | fields | meaning |
|---|---|---|---|
| server to driver | `server` | `features` (array of strings, required) | what this server takes beyond version 1; `"secrets"` means it takes `ask-secret`. Written right after the driver's `hello`, so before any `send`. |
| driver to server | `ask-secret` | `ask` (whole number from 1, required: the driver's own id, unique within its process), `account` (non-empty string, required), `what` (non-empty, required: a file name the account's provider documents, such as `"oauth-token"`), `why` (string, required, may be empty: one line, at most 200 bytes, shown in the dialog) | ask for a secret; written while a turn is open, from the AI's tool call |
| server to driver | `secret` | `ask` (required), `outcome` (required: `"stored"` or `"refused"`), `reason` (required with `refused`: `"busy"`, `"no-client"`, `"cancelled"`, `"timeout"` or `"malformed"`; older servers also sent `"unwritable"`, and `"unconfirmed"` with `stored`) | the one answer to an `ask-secret`, sent only once the value is durable or the ask has ended; never carries the value |

The driver gives its AI one tool, such as `nexus_ask_secret(account, what, why)`, which writes `ask-secret` and returns
`stored` or `refused: <reason>` from the `secret` line. The server's side (the `server` line and both kinds) is in; the
Claude driver's tool is on the roadmap (`claude-ask-secret-tool`). The AI then uses what the account can now do (its
driver starts with the credential, or the server's sign-in goes on). The design is that the AI never reads the value,
and that each driver keeps the harness's own tools out of `~/.nexus/server/accounts/`. The Claude driver does: it hands
the token to the harness on a descriptor, never in an environment, and walls the directory off from every tool its AI
has ("The token wall", below).

An `ask-secret` with an account or a `what` that is not a name, an account not set up, an account the asker's friend
is not linked to, a `what` the account's provider does not document, or a `why` that is not one line of at most 200
bytes of UTF-8 text is refused as a malformed line: no request is made, no id is minted, and the driver is ended, as
for any line the server refuses. A `why` may hold no control character (C0, DEL or C1), no bidirectional control (such
as U+202E, which would reverse the dialog's text) and no zero-width or line-separator character; the refusal names
neither the account nor the file when they are not names, so a forged line never reaches the log. The documented files
are in `src/sync/secrets.cpp` `secretFiles`:
`oauth-token` for a claude account, none yet for any other provider. An answer for an `ask` the driver no longer waits
for (its turn stopped) is skipped. A request outlives its asker: when the asker's turn is stopped or its driver exits,
the request stays open until it is answered or its deadline passes, and its `secret` line, with nowhere to go, is
dropped. A secret the person pasted is still stored.

The deadline is 10 minutes from the ask. That number is a guess, not a measurement: long enough to find a key in a
provider's console, short enough that a forgotten request does not hold an asker's turn open for long.

## Versions

A window, a server and a driver of different versions always work together; the asker learns in one plain answer
when the secret cannot be asked for.

| pairing | what happens |
|---|---|
| newer server, older window | The window agrees the older state format and never receives a request. With no window able to take one connected, the asker is answered `refused` (`no-client`: no client can take it), at once. |
| older server, newer window | The server sends no requests, so the window never shows a dialog or sends a secret action. |
| newer server, older driver | The driver skips the `server` line and never asks. |
| older server, newer driver | No `server` line arrives before the first `send`, so the driver knows the server takes no secrets, and its tool answers its AI `refused: no-client` without writing `ask-secret`. |

## Not in the model

`Secrets.tla` leaves to the code and its tests: mosh's transport and its retransmission (each action is applied once,
by number: `Link.tla`); the atomic file write and the directory's checks (the model has only a disk that fails and
heals, and a write that may or may not land); the backoff's times (the model retries whenever the disk allows); the
login's lifetime and its notices (`src/server/login_life.h`, pure and unit-tested); the refusals before a request (names, link, provider,
reason) and the per-asker `busy` (one open, 5 a minute), which mint no id and change no state the model holds; and the
asker's driver leaving while its request is open (above: the request ends by its answer or its deadline, as the
model's do).

## Binding and leak tests

**The trace binding.** `tests/sync/secrets_walk` drives the hub, the state's codec, the actions, each window's prompt,
the real `AccountFiles` (over the in-memory filesystem, at every write) and the real `SecretLines` (the server's
encoder of each driver line and each `server.log` line) at random, and TLC checks each trace against
`models/SecretsTrace.tla` with every safety property. Each record of the code's state is built from the bytes each
sink received: the driver line and the answer's reason, the session lines and every entry of the store, the log line,
and every diff sent to each window. Wherever any run of 8 bytes, hex, base64 or length of a value is found in them
(`tests/leak_scan.h`), the record holds that value's model value. `SecretsTrace.tla` checks the four no-value
properties on those code records too (`CodeNoSecretToAsker`, `CodeNoSecretInTranscript`, `CodeNoSecretInLog`,
`CodeNoSecretToWindows`), so a value that reaches a sink in the code fails TLC whatever the spec allows. The faulty
walks must be rejected: `--faulty-store-late` (a late answer stored anyway) by `TraceMatches`, and `--faulty-echo` by
`CodeNoSecretInLog`. The walk also fails and heals the disk under the keeper (a write that fails, or lands
unconfirmed), pastes a value not of a token's shape one time in five, lets answers cross deadlines and restarts the
server with values held, so `ReceivedNeverDropped`, `StoredOnlyDurable` and `NoMalformedStored` are checked on the
code's own traces. The echo is injected into the code (`SecretDesk::injectEchoFault`, a test seam that puts the
value into the log event of a stored answer), not into the walk's record.

Not in the walk yet: `Drivers`, the process table that writes each `secret` line to its driver's standard input. Driving
it needs real driver processes, a poll loop and real time per asker, which the walk (logic only, no time) cannot hold;
a fake harness that stands in for the processes inside the walk is about 400 to 600 lines (roadmap
`secrets-walk-drivers`). Until then the real-process tests below are its guard.

**The leak tests.** Each searches with `tests/leak_scan.h` (any run of 8 or more of the value's bytes, its hex in
either case, its base64 standard and URL-safe at each alignment, and its length printed as a number within 64 bytes of
the account's name), and each fake value is made up by the test:

- `tests/server/secrets_test leak` (unit): a value pasted, answered late, cancelled and dropped across three windows,
  an asker and a restart; every output (the bytes the server's own `SecretLines` writes for the driver and the log,
  the session lines, the store's files, each state sent to each window, each action's description) is searched.
- `tests/server/drivers_functional` `secret_ask` and `secret_sign_in` (functional): real driver processes, the file
  0600 in a 0700 directory, and every other file in the test's directory (the store, what the driver read, the
  driver's whole environment), the transcript and the bytes written to the driver and the log searched.
- `tests/link/link_e2e secret` (functional): the real `nexus-server`, started by the test with a HOME of the test's own
  (every e2e server is), and a window over the real link (ssh, mosh's transport, `ServerSession::take`). ada's driver
  (the fake, standing in for the claude harness) asks for a1's login; the window answers with a 7919-byte value. With
  the server stopped, every file the run left but the account's file is searched: `server.log` (the server's standard
  error and the driver's, which echoes every line it reads), the driver's environment, what it read, the store and the
  config; and every state the window held.
- `tests/server/secrets_test files_disk` (functional): a wide or linked account directory refused, a link planted at
  the temporary file's name never written through, and the start's sweep.

## The token wall

What keeps a driver's AI from reading what this dialog stores, and the login it runs on. Built in the Claude driver
(`drivers/claude/src/wall.ts`); `drivers/claude/test/functional/wall.test.ts` tries the routes below against it.

**The wall is best effort** (the owner's ruling, 2026-11-15). It is built from what the machine already has (Claude's
own sandbox, bubblewrap, socat, AppArmor) and closes the routes it has measured that it can close with those (on
Ubuntu, one AppArmor profile loaded through passwordless sudo, below); it is not isolation. A person who needs real isolation runs the AI in a sandboxed VM or container, which is on the
roadmap (`real-isolation`). What stays open is listed at the end of this section, per OS.

**What it closes.** Against every route the functional test tries, the AI the Claude driver runs does not read its
account's token or any file under `~/.nexus/server/accounts/`: not through its shell and what the shell runs, not
through Read, Write, Edit, Glob, Grep or WebFetch, and NotebookEdit is refused there too (layer 4). It does not write
there, does not reach this machine's own services by any of the spellings of this machine tried (so it does not log
in to this machine directly and read the file outside the sandbox), and does not change the files tried that nexus or
the person's programs run outside the sandbox. Permissions stay bypassed: no prompt, ever. The harness still
authenticates with the token. The test runs on macOS, Linux and WSL2, with nothing installed for it (below). A claim
marked *denied by rule, not tested* is what the wall's configuration says and no test has tried.

**The layers, and which one stops what.**

1. *The token is in no environment* (the wall for the variable). The driver hands the token to Claude on descriptor 3,
   a socket it writes and closes; `CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR` names only the number. Claude reads it
   to the newline, closes the descriptor and unsets the name before any tool runs: closing it is Claude's job, not
   the driver's, and the test shows the pinned Claude does (descriptor 3 and every open descriptor are tried). A
   Claude executable given in its place (the driver's `--claude <path>`) that does not close it leaves the token on a
   descriptor its tools inherit. No process holds the token in its environment, so `printenv`, `env`,
   `/proc/*/environ` and `ps -E` on Claude or the driver show nothing (tried).
2. *The OS sandbox around every shell command* (the wall for the files, against the shell). Claude's sandbox
   (Seatbelt on macOS, bubblewrap on Linux and WSL2) is on with no way out: `allowUnsandboxedCommands` is false, so
   `dangerouslyDisableSandbox` does nothing (tried), and `failIfUnavailable` is true, so a machine that cannot sandbox
   runs no command rather than an unsandboxed one (not a standing test; seen once in CI, 2026-11-15, when the sandbox
   could not start: every command failed with "Sandbox is required but failed to initialize"). The accounts'
   directory is denied to read and to write (`cat`, `cp`, `ln`, `grep -r`, a symbolic link and `node` tried), and the
   login variables are unset. A local socket outside the sandbox is not reachable from it: nexus-server's
   `server.sock` and an ssh agent's socket (`SSH_AUTH_SOCK`) are tried, each listening, and neither is connected to.
3. *The way out is the driver's* (the wall against this machine's own services). A sandboxed command's only
   connections leave through a proxy, and the proxy is the driver's (`drivers/claude/src/egress.ts`, HTTP and SOCKS5
   on one port on 127.0.0.1, given to Claude as `httpProxyPort` and `socksProxyPort`). It resolves every name itself,
   refuses when any address is this machine's (loopback, 0.0.0.0, `::`, any address of its interfaces, IPv4-mapped
   forms included), and connects to the address it checked. A client that does not finish its handshake in 10 s is
   dropped. Claude's own proxy refuses a name that resolves to this machine but connected to `127.0.0.1`, the LAN
   address and the tailnet address given as numbers: a sandboxed `ssh 127.0.0.1` logged in as the person, outside the
   sandbox (measured below). Every other machine is reachable: ssh (the driver's `bin/ssh`, first on Claude's PATH,
   gives plain `ssh` the proxy command Claude gives git), git over ssh, http and https. So is this machine, by way of
   another one (below, what stays open). When the proxy cannot listen, the driver refuses to start in one line.
4. *Deny rules for the file tools* (the wall for the files, against Claude's own tools, which run in Claude's process,
   not in the sandbox). `Read` and `Edit` rules on the accounts' directory, which hold in bypassPermissions mode, and
   which Claude also checks through symbolic links. Edit rules cover Write and NotebookEdit; NotebookEdit into the
   accounts' directory is refused before any rule, by Claude's own check that a notebook was read first (tried), so the
   rule on it is denied by rule, not tested.
5. *Nothing that runs outside the sandbox can be planted* (the wall around 2). Settings hooks are off
   (`disableAllHooks`; the driver's own SDK hook still runs; a hook in the project's settings is tried, and does not
   run). No tool may write the files on the wall's list of what runs outside the sandbox later (`wall.ts`
   `configFiles`; a list, not complete: below, what stays open). Tried, and denied: the session's
   `.claude/settings.json` and `.claude/settings.local.json` (by the shell and by Write), `.mcp.json` (Write, through
   `..`); nexus-server's drivers file and config (by the shell and by Write); `~/.profile`, `~/.zshenv`, a systemd user
   unit and a program in `~/bin` (by the shell); `~/.ssh`, `~/.config/git`, `~/.claude`, `~/Library/LaunchAgents`,
   `~/.gitconfig` and `~/.bashrc` (by Write); the directories of node, of the driver (with the Claude executable) and,
   on Linux, of the sandbox's bwrap and socat (by the shell and by Write); and renaming `~/.nexus` or `~/.nexus/server`
   aside (by the shell). Denied by rule, not tested: Claude's `settings.json` and `.claude.json` in the state
   directory, the other accounts' state under `~/.nexus`, every other PATH directory under HOME, the rest of what the
   person's programs run on their own (`kRunsLaterUnderHome` in `wall.ts`: fish and the login environment, tmux, vim,
   emacs, gh, docker, kubectl, aws, go and cargo config, direnv), and renaming a folder above the driver or the build
   (`<build>/sandbox`). Claude's sandbox itself keeps commands off the bash and zsh startup files, `.profile`,
   `.gitconfig`, `.npmrc`, `.envrc` and every `.git/config` and `.git/hooks`.
6. *A PreToolUse hook* (a backstop, never the wall). It refuses a call whose command, pattern, URL or path names the
   accounts' directory by an absolute spelling; a path that resolves into it or onto a file of layer 5, judged by
   every spelling (doubled slashes and `..` normalised), where it is on disk (a file not made yet is judged by its
   nearest folder that is, so a link to a protected folder with a new name is caught), and folder identity with names
   compared after NFKC normalisation and case folding (`wall.ts` `folded`): another case of a protected name, or a
   compatibility spelling of it such as the long s (U+017F), which macOS's case-insensitive volume takes for the plain
   letter while Claude's deny rules do not. That refuses more than the volume would (a fullwidth letter; another case
   on a case-sensitive volume), and never a look-alike of another script (a Cyrillic letter), which is another name on
   every volume. The functional test writes a protected file not made yet under a long-s spelling, with the hook and
   without it: Claude's rules let it through, the hook refuses it. The rest: the folder each Glob or Grep file pattern
   starts in; one
   of the directory's files under another name (a hard link); a write that makes a git directory (below); and a fetch
   of anything but an http or https URL. It reads text, and text can be spelled around; the functional test runs once
   without it to show layers 1 to 5 hold alone against the same routes.

The logins' directory holds secrets only: Claude's own state is kept in `~/.nexus/server/harness/claude/<name>/`
(its `CLAUDE_CONFIG_DIR`), so the AI keeps its memory, settings and shell snapshot, and the wall can take the logins
whole, files the dialog stores later included. An older driver kept that state in the login's directory: the first
start of this one copies only `settings.json`, `settings.local.json` and `CLAUDE.md` from there (`account.ts`
`kClaudeStateFiles`), never the transcripts, memory, shell snapshots, history or `.claude.json`, which from before the
wall may hold the token (the unit test plants a made-up token in each and finds it in nothing copied). Each of the
three is read once, from a descriptor opened without following a link (a link is not taken), and copied only when it
is a regular file of one name, at most 1 MiB, that does not hold the account's token (scanned in memory, never
logged); a hard link, a larger file or one holding the token refuses the start in one line naming the file and the
fix, and no directory is made (the unit test plants the token in each of the three and as a hard link). Those not
copied stay where they were, under the wall; a session from before does not resume.

**Linux and WSL2, with nothing installed.** The sandbox there needs bubblewrap and socat. nexus's build makes both
from pinned release sources (`cmake/sandbox.cmake`: libcap 2.78 linked in statically, bubblewrap 0.13.1, socat 1.8.1.3,
each by SHA-256) into `<build>/sandbox/`, with no root and no package; nexus-server names the directory to its drivers
(`NEXUS_SANDBOX_BIN`) and the driver puts the programs first on Claude's PATH (`drivers/claude/src/tools.ts`).
Ubuntu 24.04 and later restrict unprivileged user namespaces with AppArmor
(`kernel.apparmor_restrict_unprivileged_userns = 1`): a program makes them only when a profile attached to its path
allows it, so nexus's bwrap is refused (`setting up uid map: Permission denied`). The system's `/usr/bin/bwrap` is no
way round it: its profile confines what it runs, and the sandbox's own seccomp step, which needs a user namespace inside
the sandbox, then fails, so no command runs (measured on an Ubuntu 26.04 bench, 2026-11-15). So on such a kernel the
driver loads an AppArmor profile for nexus's own bwrap, once per machine and build directory, automatically and with
no prompt, through passwordless sudo: `/etc/apparmor.d/nexus-bwrap-<hash of the path>`, attached to that one path,
allowing user namespaces and nothing else (`tools.ts` `appArmorProfile`, as Ubuntu's own profiles for Chrome and
Electron apps do), loaded with `apparmor_parser -r` and then kept where AppArmor loads it at boot. The profile's text
goes to both commands on standard input, never through a file (a file the driver wrote would be one the sandbox can
write too; `tee` writes it, then `chmod`, and the driver checks the file holds the text), and `sudo`,
`apparmor_parser`, `tee` and `chmod` are run by absolute path, never looked up on a PATH (the unit
test checks no other path is ever handed to root). Before anything runs as root the driver checks that
`/etc/apparmor.d` is a folder (not a link) owned by root that no one else can write (0755 or stricter), and refuses
to start in one line otherwise (unit-tested). A load logs one line saying so. Claude's sandbox still refuses the AI's
own commands a user namespace. Without passwordless sudo the driver refuses to start in one plain line naming what is
missing; it never changes the sysctl, which would lift the restriction for every program. The profile trusts whatever
program is at that path, which the person's own account can replace: it gives that path what the restriction takes
from every unprivileged program, and nothing more. WSL2's kernel has no AppArmor: nexus's own bwrap runs there as it
is (measured on captain).

On Linux and WSL2 the sandbox also needs `~/.bashrc` and `~/.gitconfig` to exist: Claude's sandbox keeps commands from
writing them, and one that is missing makes every bash or git command fail ("Permission denied"). The driver makes
each one that is missing, empty, before Claude starts (`tools.ts` `sandboxHomeFiles`); bash and git read an empty one as
none. One it cannot make refuses the start in one line naming it. And plain `ssh` in the sandbox goes through
`bin/ssh-proxy`, which hands socat an IPv6 address in brackets, so that `ssh ::1` reaches the driver's proxy and is
refused there, as every other spelling of this machine tried is.

**What it costs the AI's shell.**
- *git init and clone stay refused.* Claude's sandbox never lets a command write a `.git/config` or `.git/hooks`, so
  `git init`, `git clone`, `git config` and `git push -u` fail in it (commit, branch, fetch, pull and push in a
  repository that exists work). Letting the driver make repositories for the AI (an empty, read-only
  `core.hooksPath` and the config sealed after creation) would not make this sound, because the danger is not the new
  repository's own hooks: it is any git directory the AI controls that the person's git later reads, which a command
  can make without init or clone (below, what stays open). Making repositories for the AI would only add more such
  directories.
- *ssh works to every other machine; mosh does not.* mosh's session is UDP straight to the server, and the sandbox
  lets nothing out but TCP through the proxy (Seatbelt allows only the proxy's port; bubblewrap's network namespace
  has no route), with no setting in Claude to open UDP; mosh itself has no proxy support. The AI's ssh is the same
  login without the roaming.
- *Writes* go to the home directory, the working directory and the temporary directories, less the files of layer 5.

**Measured** (macOS, the pinned SDK's Claude 2.1.322, a fake token, `NEXUS_WALL_REPORT=1 npm run test:functional`;
the same test runs in CI on Ubuntu 26.04 benches and in captain's WSL2, and the report gives each try's time). Before
the wall, 15 of the 27 first tries read the token (and three wrote over it or planted a hook), and a sandboxed ssh to
this machine logged in as the person; with it, none of the routes below does, with the hook and without it.

| route | before | with the wall |
|---|---|---|
| shell: `printenv`, `env` | not read (Claude scrubs its children) | not read |
| shell: the parent's environment (`ps -E`) | **read** | not read (layer 1) |
| shell: descriptor 3 and every open descriptor | not read | not read (layer 1) |
| shell: `cat`, through a glob, `grep -r ~`, `node` reading it | **read** | denied (layer 2) |
| shell: a hard link, a copy, a symbolic link | **read** | denied (layer 2) |
| shell: `dangerouslyDisableSandbox` | **read** | denied (layer 2) |
| shell: overwrite the token | written | denied (layer 2) |
| shell: connect to nexus-server's socket, an ssh agent's | not tried | denied (layer 2) |
| ssh to this machine by `127.0.0.1`, `2130706433`, its tailnet address | **logged in** | refused (layer 3) |
| ssh to this machine as `localhost`, `::1`, `127.1`, `0.0.0.0`, its name, its LAN address | its name refused; its LAN address reached sshd; the rest not tried | refused (layer 3) |
| ssh to another machine (hulk), and from there back to this one | plain `ssh` failed; with Claude's proxy command, reached | plain `ssh` reached; back refused by the fleet (publickey) |
| plant: nexus-server's drivers file and config, `~/.profile`, `~/.zshenv`, a PATH program | writable (HOME was) | denied (layer 5) |
| Write: `~/.ssh`, `~/.config/git`, `~/.claude`, a login agent, `~/.gitconfig`, `~/.bashrc` | not tried | denied (layer 5) |
| shell and Write: beside node, the driver, the sandbox's tools; rename `~/.nexus` aside | not tried | denied (layer 5) |
| Read: the file, through a symbolic link | **read** | denied (layer 4; 6) |
| Read: a hard link | **read** | never made (layer 2) |
| Grep: under HOME, in the working directory | **read** | not found (layer 4; 6) |
| Glob: under HOME | listed | not found (layer 4; 6) |
| Write, Edit: the token; a new file there by a doubled slash | written, tried | denied (layer 4; 6) |
| NotebookEdit: a notebook there | not tried | refused (Claude: not read first) |
| WebFetch: a `file://` URL, a local URL | not read | refused (6); invalid URL |
| a hook in the project's settings | **read** | never runs (layer 5) |
| Write a hook into the local settings | written | denied (layer 5; 6) |
| Write: a settings file or `~/.zshenv` not made yet, spelled with a long s | written (and without the hook) | refused (6) |
| the harness authenticates with the token | yes | yes, from the descriptor |

**Git directories, by name.** The file tools (Write, Edit, NotebookEdit) may not create or change a repository's
`.git/config` or hooks, or a `commondir`, anywhere: deny rules (`wall.ts` `kGitNameRules`), which Claude also applies to
every shell command on macOS. Nor a `.git` file or directory, anything in one, or a `HEAD`: the hook refuses those
(`kGitNames`), because as deny rules they would reach the macOS sandbox too, and there they stop every commit (git
writes `.git/index.lock` and `.git/logs/HEAD`; measured, 2026-11-15). The functional test writes each and is refused.
What it costs:
- *The file tools:* no Write or Edit of anything in a `.git`, or of a file named `HEAD` or `commondir`; the AI uses git
  itself for those.
- *The shell, on macOS:* no write of a `.git/config`, a hook or a `commondir`, so `git worktree add` fails there
  (commit, branch, fetch, pull and push still work, as the test checks).

**What stays open, per OS.**
- *Every OS: this machine by way of another.* The sandbox can read `~/.ssh`, the person's keys among it, and reach
  every other machine. A jump through a host that trusts the person's key (or a key there that this machine trusts)
  reaches this machine's sshd as the person, outside the sandbox. The test's ssh-out probe sees the fleet refuse the
  way back today; that is the fleet's configuration, not the wall. An option that closes it: deny the sandbox reading
  the person's private keys in `~/.ssh` (a deny-read rule beside the accounts'). Its cost: the AI cannot ssh, or use git
  over ssh, with the person's keys at all, only with keys of its own. Not done in this change.
- *Every OS: descriptor 3 is Claude's to close* (layer 1). A substituted Claude executable that ignores it leaves the
  token on a descriptor its tools inherit.
- *Every OS: the hook is text-based* and can be spelled around (layers 1 to 5 hold without it against the routes the
  test tries).
- *Every OS: a planted git directory.* The shell can write a `.git` file or a `HEAD` wherever it may write, so a
  command can make a directory the person's own git later reads as a repository and runs what its config names
  (above, git init and clone). Not a read by any of the AI's tools: it needs the person to run git in that directory.
  On macOS Claude applies the git deny rules to the shell too; it does not see a `.git` file pointing elsewhere.
- *Linux and WSL2:* the planted-git-directory route through the shell stays open: closing it there needs root. Whether
  the Linux sandbox applies the name rules above to shell commands is not measured; nothing here relies on it.
- *WSL2: the Windows host.* The proxy refuses this machine's own addresses; the Windows host, at WSL2's gateway address
  in its default (NAT) networking, is another machine to it, and reachable, with whatever it serves (an ssh server
  there that trusts the person's key above all). Not measured; nor is mirrored networking, where the two share
  addresses.
- *Every OS: the hook checks a path, then Claude opens it.* A command run between the two can swap a link along the
  path (a folder checked as harmless made a link into a protected one), and the tool then opens what the link names.
  Layers 2 and 4 still stand behind the hook for the accounts' directory; for the files of layer 5 that Claude's rules
  do not name by every spelling, the hook may be the only check, and this race goes round it (not tested).
- *Every OS: the list of what runs outside the sandbox is not complete.* Layer 5 names the common shells' start-up
  files, login agents, and the config of git, ssh, tmux, editors and the tools above; it does not name every file a
  program the person runs later reads commands from. Not on it, for example: `~/.bash_aliases` and other files a
  start-up file sources, window-manager and desktop config files whose lines run commands, and shell frameworks'
  custom folders (such as oh-my-zsh's `custom/`). The shell and the file tools may write those.
- *Linux with a restricting AppArmor:* the profile trusts the program at its path, which the person's account can
  replace (above). The driver trusts root's `/etc/apparmor.d` once it has checked it is root's alone. A profile is
  kept for each build path nexus ever ran from, and the old ones stay in `/etc/apparmor.d` until the person removes
  them (for each old one, `sudo apparmor_parser -R <file>`, then `sudo rm <file>`).
- *nexus's own CI:* the driver's packages and the sandbox tools are kept per runner under `~/.cache/nexus-ci`, and a
  job on any branch can write them, so a branch's job can change what a later job of another branch runs. Not the
  product; CI only.
- *The driver's own memory.* nexus-server and the window set `RLIMIT_CORE` to 0 and, on Linux, mark themselves not
  dumpable (`src/no_core.h`, above). The Claude driver is a Node process, and Node has no call for either, so it does
  neither: from reading the token file until Claude has it, the token is in the driver's memory, and a core file of it
  or a process of the same user allowed to trace it could hold or read it.

Not a hole, a known fault: on Linux two Claude sandboxes at once, whose working directories share an ancestor a few
levels up, now and then fail each other's commands (`bwrap: Can't find source path <ancestor>/.mcp.json`; measured in
CI, 2026-11-15). The command fails closed. Only the tests avoid it (each CI job's runs go under a temporary directory
of its runner's own, one run at a time); nexus does not yet.
