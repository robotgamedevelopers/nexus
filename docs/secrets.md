# The secret dialog

How a secret (an API key, a token) gets from a person's fingers to its file on `nexus-server`, with no AI ever
seeing it. The owner's rulings (2026-11-14): everything about logging in and staying logged in is built into the app;
a service that needs an API key gets one paste, in the app, and it just works; the client does the sign-in and sends
the server what it needs; the same holds for any secret nexus asks for; a little dialog pops up, nobody sees the
secret, it goes to the server, and it is handled there. With AGENTS.md: a secret lives under
`~/.nexus/server/accounts/<name>/` (0700 directories, 0600 files) and is never shown, logged, or seen by an AI.

The state machine is `models/Secrets.tla` (`models/README.md` has its properties, instances and witnesses). This page
is the design the code is built to, after the accounts layer (roadmap 5b); the code is bound to the model by a trace
check when it lands.

## The flow

1. An **asker** asks `nexus-server` for a secret for an account: a friend's AI, through its driver (below), or the
   server itself (the sign-in flow, roadmap 5c).
2. The server refuses at once, answering the asker `refused`, when a request is already open for that account
   (`busy`), or when no connected window can take a request (`no-client`: none connected, or only windows from before
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
too (the server keeps the next id with its other durable counters), so a dialog left open across a restart can never
satisfy a later request. A window that loses its link or quits loses its dialog and anything typed in it; the request
stays open for the other windows, or until its deadline.

The asker hears exactly one answer per request: `stored`, or `refused` with one reason of `busy`, `no-client`,
`cancelled` or `timeout`. Never the value, never its length, never which window answered.

## The dialog

- It belongs to the overlay, like any pop-up, on whatever tab shows. It names the account, what is asked and who asks
  (a friend and session, or nexus itself), with the asker's one-line reason shown as quoted text: it is data, never an
  instruction to the person.
- nexus never shows itself for a request (the launch rule: only the hotkey shows the overlay). A request that arrives
  while the overlay is hidden waits for the next show.
- One field, masked: a fixed row of dots, whatever the length. One paste (Command-V) fills it; typing works too. Enter
  sends; Escape cancels the request, for every window (Escape backs out one context, and this dialog is one, on a
  terminal tab too).
- Nothing echoes the value: no copy or cut out of the field, no undo history, no drag, no accessibility value. It
  never enters the window's draft state, its saved preferences, a state file or a screenshot (`--screenshot` draws the
  dialog with its dots only). After Enter or Escape the field is cleared and its memory overwritten.
- When the request ends elsewhere (answered in another window, cancelled, past its deadline), the dialog closes. The
  outcome shows as one line in the asker's session (`stored`, or `refused` and why), with no value.

## The wire

The value travels in exactly one place: one action, from the window that took it to `nexus-server`, over the link's
mosh transport (encrypted with the session's key). Nothing else on the wire, in any direction, carries it.

- **Server to window.** The sessions state gains the open requests, after the sessions: each one framed by its
  length (`docs/formats.md` rule 1) and holding the request id, the account, what is asked, the asker (a session id,
  or 0 for nexus itself) and the asker's one-line reason. Never a value. This is a new state format (3), agreed when
  the window connects as today; a window that agrees an older format receives no requests, and the server counts it
  as unable to take one.
- **Window to server.** Two new actions: a secret answer (the request id and the value) and a secret cancel (the
  request id). The actions are unframed today, and `docs/formats.md` says their first change comes with a framed
  format agreed the same way: the actions become framed with state format 3, and a window sends these two only for a
  request it received, which no older server ever sends.
- **The value in the window.** It is held in the action until mosh's transport has the server's acknowledgement, and
  then the subtract that drops acknowledged actions drops it; the window overwrites its own copy. Actions are never
  written to disk, and their debug description gives a secret answer's kind and request id only.
- **The value in the server.** The applier hands a secret answer to the request's handler, which writes
  `accounts/<name>/<what>`: the directory made 0700 if missing, the value written to a temporary 0600 file beside it,
  synced and renamed over the old one. It never goes into the store, a record, a snapshot, the state, a transcript, a
  driver's input or `server.log`; the log line for an answer is the request id, the account and the outcome. The
  request itself is not durable (it holds no secret, and it ends with the server).

## How an AI asks: the driver protocol

These are additions to `docs/driver-protocol.md`, version 1: each is a kind an older reader skips (rule 8), so no
version change. They land with the code, in the same pull request as `src/driver/protocol.h`.

| direction | kind | fields | meaning |
|---|---|---|---|
| server to driver | `server` | `features` (array of strings, required) | what this server takes beyond version 1; `"secrets"` means it takes `ask-secret`. Written right after the driver's `hello`, so before any `send`. |
| driver to server | `ask-secret` | `ask` (whole number, required: the driver's own id, unique within its process), `account` (non-empty string, required), `what` (required: a file name the account's provider documents, such as `"api-key"`), `why` (string, required: one line, at most 200 characters, shown in the dialog) | ask for a secret; written while a turn is open, from the AI's tool call |
| server to driver | `secret` | `ask` (required), `outcome` (required: `"stored"` or `"refused"`), `reason` (required with `refused`: `"busy"`, `"no-client"`, `"cancelled"` or `"timeout"`) | the one answer to an `ask-secret`; never carries the value |

The driver gives its AI one tool, such as `nexus_ask_secret(account, what, why)`, which writes `ask-secret` and returns
`stored` or `refused: <reason>` from the `secret` line. The AI then uses what the account can now do (its driver
starts with the credential, or the server's sign-in goes on); it never reads the value, and each driver keeps the
harness's own file tools out of `~/.nexus/server/accounts/` (roadmap 5d, with the drivers taking the account).

An `ask-secret` with a `what` the account's provider does not document is refused as a malformed line (no request is
made). An answer for an `ask` the driver no longer waits for (its turn stopped) is skipped. A request outlives its
asker: when the asker's turn is stopped or its driver exits, the request stays open until it is answered or its
deadline passes, and its `secret` line, with nowhere to go, is dropped. A secret the person pasted is still stored.

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
by number: `Link.tla`); the atomic file write; a check on the value's shape (if one is added, it joins the model as a
refusal reason with the code); and the asker's driver leaving while its request is open (above: the request ends by
its answer or its deadline, as the model's do).
