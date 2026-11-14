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
6. The server takes the first answer for an open request, against the request's state when it processes it: it
   writes the account's file and answers the asker `stored`; a cancel answers it `refused` (`cancelled`). A request
   nobody answers in time ends `refused` (`timeout`).
7. The request leaves the state, and every other window closes its dialog when its state no longer holds it.
8. An answer for a request that is no longer open (answered in another window, cancelled, past its deadline, or from
   before a restart) is dropped: nothing is stored, and the log records the request id and `late`, never the value.

A server restart drops every open request without an answer: its asker is gone with it (a driver ends with the server,
`docs/driver-protocol.md`), and the server's own sign-in asks again. Request ids are never reused, across restarts
too: the store keeps the last one minted as the applied counter of a client named `nexus:secret-requests`, a name no
window can take, committed before the request opens; so a dialog left open across a restart can never satisfy a later
request. A window that loses its link or quits loses its dialog and anything typed in it; the request
stays open for the other windows, or until its deadline.

The asker hears exactly one answer per request: `stored`, or `refused` with one reason of `busy`, `no-client`,
`cancelled`, `timeout` or `unwritable` (the value arrived and its file could not be written; the log says why, from the
filesystem's error, never the value). Never the value, never its length, never which window answered.

`stored` goes out only once the file is in place: the value is written to a temporary 0600 file, synced, renamed over
the old one and the directory synced before the hub hears it was kept, and the asker's driver line is held, with
everything else the server's pass made, until the pass's flush (`server/pass.h`). A crash at any point leaves the old
value or the new one, never part of one (`secrets_test files_crash`). If the rename succeeded and only the directory's
sync failed, the answer says what is true: `stored` with the reason `unconfirmed` (the value is in its file; a crash
before the disk settles may bring the old file back), the session line says so, and `server.log` gives the
filesystem's error. A temporary file a crash or a failure left behind holds a value no one reads: the server removes
every `*.new` in every account's directory when it starts, before anything is asked, and logs each one it removed.

The account's directory is the server's alone. Before a write the server checks it with `lstat`: a directory that is a
symbolic link, is not a directory, belongs to another user, or is open to group or others (anything wider than 0700)
is refused, `unwritable`, with the fix in the log; the server never follows a link there and never widens or narrows
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
  request itself is not durable (it holds no secret, and it ends with the server).

## How an AI asks: the driver protocol

These are additions to `docs/driver-protocol.md`, version 1: each is a kind an older reader skips (rule 8), so no
version change. They land with the code, in the same pull request as `src/driver/protocol.h`.

| direction | kind | fields | meaning |
|---|---|---|---|
| server to driver | `server` | `features` (array of strings, required) | what this server takes beyond version 1; `"secrets"` means it takes `ask-secret`. Written right after the driver's `hello`, so before any `send`. |
| driver to server | `ask-secret` | `ask` (whole number from 1, required: the driver's own id, unique within its process), `account` (non-empty string, required), `what` (non-empty, required: a file name the account's provider documents, such as `"oauth-token"`), `why` (string, required, may be empty: one line, at most 200 bytes, shown in the dialog) | ask for a secret; written while a turn is open, from the AI's tool call |
| server to driver | `secret` | `ask` (required), `outcome` (required: `"stored"` or `"refused"`), `reason` (required with `refused`: `"busy"`, `"no-client"`, `"cancelled"`, `"timeout"` or `"unwritable"`; optional with `stored`: `"unconfirmed"`, the value is in its file but its durability is not confirmed) | the one answer to an `ask-secret`; never carries the value |

The driver gives its AI one tool, such as `nexus_ask_secret(account, what, why)`, which writes `ask-secret` and returns
`stored` or `refused: <reason>` from the `secret` line. The server's side (the `server` line and both kinds) is in; the
Claude driver's tool is on the roadmap (`claude-ask-secret-tool`). The AI then uses what the account can now do (its
driver starts with the credential, or the server's sign-in goes on). The design is that the AI never reads the value,
and that each driver keeps the harness's own file tools out of `~/.nexus/server/accounts/` (roadmap 5d, with the
drivers taking the account). That is not true yet: the Claude driver passes the account's token to the harness in the
harness's environment, where the AI's own tools can read it. The fix, keeping the token out of anything the AI's tools
can reach, is its own pull request, in `drivers/claude`.

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
by number: `Link.tla`); the atomic file write and the directory's checks; a check on the value's shape (if one is
added, it joins the model as a refusal reason with the code); the refusals before a request (names, link, provider,
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
`CodeNoSecretInLog`. The echo is injected into the code (`SecretDesk::injectEchoFault`, a test seam that puts the
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
