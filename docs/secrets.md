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

1. An **asker** asks `nexus-server` for a secret for an account: an AI, through its driver (below), or the
   server itself (a login for an account whose provider has no in-app sign-in; a claude account signs in in the
   browser instead, "Sign-in" below).
2. The server refuses the ask outright (below: no request, no id) unless the account and what is asked are each a name
   (letters, digits, `-`, `_` and `.`, at most 64), the account exists, the asker's AI is linked to it in
   `ais.json` (an AI asks only for the accounts it may use; nexus itself may ask for any), the account's
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
| `malformed` | the person's input | told why (never what); asked again: nexus's own asks at once, an AI by its own ask |
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
(`oauth-token` for a Claude account without a configured email), the account counts as signed in at once, and each session held for want of a
credential starts its driver for the prompt waiting, without a restart (`Supervisor.tla` `SignIn`).

## The dialog

- It belongs to the overlay, like any pop-up, on whatever tab shows. It names the account, what is asked and who asks
  (an AI and session, or nexus itself), with the asker's one-line reason shown as quoted text: it is data, never an
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
  or 0 for nexus itself), the asker's AI, and the asker's one-line reason. Never a value. In format 3 the list is
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
`stored` or `refused: <reason>` from the `secret` line. Both the server's side (the `server` line and both kinds) and
the Claude driver's tool (`drivers/claude/src/driver.ts` `secretMcpServer`, an in-process SDK MCP server exposing
`nexus_ask_secret` and checking `account`, `what` and `why` locally before writing `ask-secret`) are in. The AI then uses what the account can now do (its
driver starts with the credential, or the server's sign-in goes on). The design is that the AI never reads the value,
and that each driver keeps the harness's own tools out of `~/.nexus/server/accounts/`. The Claude driver does: it hands
the token to the harness on a descriptor, never in an environment, and walls the directory off from every tool its AI
has ("The token wall", below).

An `ask-secret` with an account or a `what` that is not a name, an account not set up, an account the asker's AI
is not linked to, a `what` the account's provider does not document, or a `why` that is not one line of at most 200
bytes of UTF-8 text is refused as a malformed line: no request is made, no id is minted, and the driver is ended, as
for any line the server refuses. A `why` may hold no control character (C0, DEL or C1), no bidirectional control (such
as U+202E, which would reverse the dialog's text) and no zero-width or line-separator character; the refusal names
neither the account nor the file when they are not names, so a forged line never reaches the log. The documented files
are in `src/sync/secrets.cpp` `secretFiles`:
`oauth-token` for a Claude account without a configured email. A Claude account with a configured email requires verified browser enrollment. An answer for an `ask` the driver no longer waits
for (its turn stopped) is skipped. A request outlives its asker: when the asker's turn is stopped or its driver exits,
the request stays open until it is answered or its deadline passes, and its `secret` line, with nowhere to go, is
dropped. A secret the person pasted is still stored.

The deadline is 10 minutes from the ask. That number is a guess, not a measurement: long enough to find a key in a
provider's console, short enough that a forgotten request does not hold an asker's turn open for long.

## Browser sign-in for Codex, Antigravity, Grok and Muse

Each account has a separate directory under `~/.nexus/server/harness/<provider>/<account>/`.
The server reads the provider's `signin` command from its drivers file and supplies
the selected account name. The window connected to that server over loopback opens
the helper's one-time local page in the user's browser. The provider's device link,
code or pasted response stays in that browser flow. The window claims a flow and
opens its page through the existing sign-in protocol.

When Studio offers a Google account page for Antigravity, the MacBook opens it directly in the default browser. Paste Google’s return code into the masked field on Emma’s tab. The field sends the code only to the enrollment the window owns.

On macOS and Linux, a remote Codex or Antigravity sign-in offering a private helper page opens that page through SSH.
The client uses its configured server destination and requires a trusted SSH host key. It binds only
the matching IPv4 loopback port, waits for SSH to confirm the bind, and then opens the browser.
The page path stays out of SSH arguments. Ending the flow, losing the connection, or locking the
client closes the forward. An occupied port produces a retry message on the sign-in card.

The server accepts a successful helper exit for the current flow only after it
verifies the account's owner-only enrollment receipt. That receipt contains the
provider name. Each adapter separately checks subscription access before sending
a prompt. The account ID selects the credential directory. Its short label and
email are descriptive configuration that can change independently of that ID.
See [the driver configuration](../drivers/rpc/README.md) for the launch contract.

## Sign-in: a claude account, in the browser

The sign-in card names the selected account and shows its configured email. Enter or a click opens the browser.
Nexus keeps the account's login in its private server directory. The browser URL, callback code and credential
stay outside the conversation, logs and fork configuration.

When a remote client explicitly starts signing in to a different account, it declines the previous account's
unfinished browser callback and releases its listener. It sends the existing state-matched denial callback;
the server handles it as a cancelled login. A callback already received is forwarded first and is never replaced
by a denial. Choosing the same account again reopens its waiting page, including after the browser was closed.
Merely changing tabs or hiding the overlay does not cancel sign-in. `SignIn.tla`'s `Cancel` action and the
implementation trace cover the handoff; `frame_host_unit` covers switching accounts and reopening the browser.

### Verified Claude sign-in

When a Claude account has an `email`, the server starts its trusted `nexus-claude-auth` helper for that account.
The helper invokes `claude auth login --claudeai --email <expected>` in a fresh home and config directory.
On macOS the sandbox blocks shared Keychain services. During enrollment and renewal, a private
`security` command reports an empty, read-only Keychain so Claude can use its account-private file store.
Passing the email to the browser flow
requests an identity. To verify who actually signed in, the helper runs `claude auth status --json` in the
same isolated directory and checks the completed first-party Claude login. It requires a matching email
(ASCII case-insensitive) and a usable subscription grant from that flow. A mismatch or a missing identity
leaves the existing stored grant intact. The person can select the correct browser account and try again.

On a match, the helper writes the confirmed email and the grant together in the account's private
`verified-claude.json`. The trusted driver obtains an access token through the helper before submitting a
prompt. The helper checks the stored email against the configured email and uses the Claude CLI to renew
an expiring access token from that grant. A slot with a configured email cannot be authorized by pasting
a token or using a legacy `oauth-token` file. Use a window with browser sign-in support to enroll it.

For Codex, Grok, Muse and Antigravity, `/logout` removes the named harness login and enrollment receipt.
A local `/logout` on a Claude account removes its verified grant, legacy token and renewal files
and retires idle drivers using that login. The command requires the account's active work and sign-in to
finish or be cancelled. It leaves other slots and the browser's Claude session independent.
See [account selection and sign-in](accounts.md#signing-in-to-the-right-claude-account).

### Legacy Claude sign-in without a configured email

For a slot without an email, the server accepts the long-lived token printed by Claude's
`setup-token` flow. The steps below describe that flow.

The state machine is `models/SignIn.tla`, bound to the code by `tests/sync/signin_walk` and `models/SignInTrace.tla`
(`models/README.md`). The code: `src/sync/signin.h` (the server's `SignInDesk`, the window's `SignInOpener` and
`CallbackGate`, the scanners), `src/sync/hub.cpp` (the desk in the hub), `src/server/signin_runner.h` (the processes),
`src/server/signin_open.cpp` (the URL helper), `src/signin_gate.h` (the window's gate) and `src/frame_host.h`
(`LinkSignIn`, the window's side).

**The flow.**

1. **Needed.** The server marks a claude account needed when it has no login: at the start (none stored), when its
   driver reports no credential or its login ends, or when an AI asks with `nexus-server config sign-in <account>`.
   That mints a flow id, durable and never reused across restarts (the store's applied counter of the client
   `nexus:sign-ins`), and offers the flow in the state every window receives. One flow per account at most; none while
   a login for it is held in the one-chance store. Each session on the account gets one notice: "needs signing in:
   show nexus's window (its hotkey) to sign in in the browser". The AI that asked gets one line back, `signed in` or
   `refused: <why>`, and nothing of the flow.
2. **Claimed.** A window claims a flow only once the person has shown it (the hotkey: the person is there), and only
   the oldest offer no window holds that it has not claimed before. Each window claims by a claim token of its own,
   32 random letters and digits made at launch; the state names a flow's holder to the holder alone and shows every
   other window `*`, so no window learns another's token, and a second window or a reconnect never starts a second
   flow. The first claim the server takes for a needed flow starts its process; a claim of a running flow whose
   window's link ended takes it over without a second process; any other claim is late and changes nothing.
3. **The process.** The server starts the unmodified `claude setup-token` (the `claude` program the server's config
   names, else the first on PATH) under a pseudo-terminal of its own, with an environment made from nothing but HOME,
   PATH, TERM, LANG, a CLAUDE_CONFIG_DIR of its own under the account's directory (0700, removed when the process
   ends), and BROWSER. No API key variable, nor anything else of the server's environment, reaches it. First on its
   PATH is nexus's helper, `nexus-signin-open` (beside `nexus-server`), as `open`, `xdg-open`, `wslview`,
   `sensible-browser`, `www-browser` and `x-www-browser`, and it is BROWSER too: it hands the URL to the server over a
   Unix socket in the server's directory (0600), with a nonce that names its flow, and opens nothing. Where the program
   opens a browser by a path of its own, the URL it prints in its terminal is taken instead (`UrlScanner`, which reads
   the terminal's text as a terminal shows it: CSI, OSC, DCS and the other strings, the two-byte and the
   charset-select forms, each split across reads, are never text, and every one of them, a colour change too, ends a
   URL's run; a URL counts once its line stands, at its line feed, and a carriage return followed by anything else
   starts the line over). A URL fails the flow unless it is https, to a host on Claude's sign-in list (`claude.com`,
   `platform.claude.com`, `claude.ai`, compared byte for byte, so a look-alike such as `claude.com.example` is
   refused), of printable bytes only, with a local redirect (http, localhost or 127.0.0.1, a port) and a state. The
   window checks the host against the same list before it opens anything.

   *Two things the process holds that are not nexus's choice.* `claude setup-token` itself starts the helper as
   `open <url>`, so the URL is in the helper's argv, which another process of the same user can read while the helper
   runs (a few milliseconds); nexus never puts the URL in an argv of its own. And the helper's nonce comes to it in its
   environment (`NEXUS_SIGNIN_FLOW`, `<flow>-<32 random letters and digits>`), since the helper is the program's child
   and the program passes its environment down: there is no private channel to it. The nonce is not a secret of the
   sign-in: it only says which flow a URL is for, the socket is 0600 in a 0700 directory, and the runner compares it in
   constant time. The residual route is a process of the same user that reads the helper's environment and connects
   to the socket first: it can hand over a URL of its own, which must still pass the host list, and it already runs
   as the user.

   *A crash.* The runner locks `.signin/lock` and gives the lock to the process (its fd 3), with the process's group in
   `.signin/pid`. A server that starts after one that ended without its runner's end (a crash, a SIGKILL) ends every
   process still holding such a lock (its group: SIGTERM, then SIGKILL after 2 s) and removes every `.signin` left
   under the accounts, one line each in `server.log`; a process that does not hold the lock is never touched.
4. **Opened.** The server puts the URL in the holding window's state alone. That window opens it in the system browser
   once, and says so in one action, which carries no URL. The open runs on a thread of the window's own
   (`src/url_opener.h`), never the frame's: the frame only queues the URL, and the queue is wiped as it goes.
5. **The callback.** On the server's machine (the window reaches the server at a loopback address, or the server's host
   name is the window's own), Claude Code's own listener takes the browser's redirect, and the window binds nothing.
   On another machine, the window first binds a gate on 127.0.0.1 (and ::1 where it can) at the redirect's port, on a
   thread of its own. The gate takes exactly one GET on the redirect's path whose `state` matches the URL's and that
   carries a `code` or an `error`; it answers that one with a plain page ("Sign-in response received. Return to Nexus."), sends
   its query up in one action, keeps nothing, and closes. Receiving a callback wakes the app; it forwards the
   response and applies the server’s result even when the overlay is hidden behind the browser. The page confirms
   receipt only: the server still has to verify and save the login. Any other request is answered "not found" and changes
   nothing. The gate compares the state in constant time, refuses a query with a control byte (CR, LF or any other
   byte outside 0x21 to 0x7e), holds at most 8 connections at once and drops each one 10 s after it opened (both
   guesses, not measurements). Where [::1] exists but its port is taken by another program, the gate fails with one
   plain line rather than listen on 127.0.0.1 alone (a browser that tries ::1 first would hand the callback to that
   program). The server replays the query once, from the holding window only, while the flow waits, only with the
   URL's state (compared in constant time) and only of printable bytes, to the listener on the redirect's own host and
   port, on loopback alone: `localhost` on [::1] first, then 127.0.0.1, as a browser tries it; `127.0.0.1` on IPv4
   alone; anything else is dropped. The token exchange that follows gets 30 s past the flow's deadline (a guess, not a
   measurement) before its process is told to end. The gate also closes when its flow leaves the window's state or the link is lost. A window binds
   one gate at a time: a second page that needs one waits, queued, until the first's callback is taken or its flow
   leaves.
6. **Captured.** The process prints the token. The server takes it from the terminal (`TokenScanner`, which holds at
   most one login's bytes at a time and wipes them, and reads the terminal's text as `UrlScanner` does, but for a
   colour change, which a run of the token goes on across). A candidate is a run of the subscription login's shape,
   `sk-ant-oat01-` and its token bytes, exactly 108 bytes in all (the published shape, not a measured login: a run of
   another length is passed over, and its length, never its bytes, is logged); a longer run is two things joined and
   no candidate. It counts once its line stands; when the process has ended, by itself with status 0 or by the
   server's stop, the last candidate printed is the login. The server checks again that it is a subscription's login,
   not an API key. It then puts the token straight into the one-chance store as a request of nexus's own (above: never
   dropped, stored when durable, held and written again while the disk refuses it). The account is signed in, and
   each session held for want of a credential starts its driver. Sessions on the account read "was signed in in the
   browser". A token printed after its flow ended (the deadline passed while the browser was sent back, or in the
   grace after the process was told to end) is stored all the same, and a needed flow for the account ends dropped:
   the person signed in, so nothing asks again (`models/SignIn.tla` `EveryPrintTaken`).

**How a flow ends.** Each end is one state and one plain notice in the account's sessions; each but captured and
dropped makes a fresh flow at once, so the next show of the window starts the sign-in again.

| end | when | notice |
|---|---|---|
| captured | the token was printed and is a subscription's login | "was signed in in the browser" |
| cancelled | the person declined on the page, and the window's gate forwarded that | "declined on the page" |
| failed | the process ended without a token (a decline on the server's own machine is this: the server never sees the redirect), its URL was refused, or what it printed was not a subscription's login | "ended without a login" |
| expired | 10 minutes after its process started (a guess, not a measurement; 30 s more once the callback was forwarded, while the program exchanges the code for its login), its process ended | "was not finished in 10 minutes" |
| dropped | the server ended; the account was signed in otherwise (a paste, its driver, a late token); or only windows from before sign-ins are linked, and a paste is asked for instead (Versions, below) | none |

A window that closes or loses its link lets go of the flows it holds; the process keeps running, and the next other
window shown takes the flow over and opens the URL again. The same window, back, does not claim a flow it claimed
before: that flow ends at its deadline, and the fresh one after it is offered at the next show. When the windows left
linked are all from before sign-ins, a flow no window holds ends at once, its process ended, and a paste is asked for
instead (Versions, below). A window's gate that cannot bind its port shows one problem line in that window.

**The window.** Nothing new shows: no settings screen, no hint text. While an account has no login, its sessions'
state cursor is red (AGENTS.md, "The state cursor"), and the notices above are the only words. All of the window's
work for a sign-in is a non-blocking step each frame (the gate's sockets and the browser's open are on threads of
their own); the frame-budget test's `sign_in` case drives a claim, an open through the real opener thread over a stub
browser that takes 300 ms, a real gate and the flow's end within 8.3 ms a frame.

**Where each value travels, and where it never goes.**

| value | where it is | never in |
|---|---|---|
| the URL | the process's terminal, or the helper's argv (put there by `claude setup-token` itself) and the helper's socket (0600); the server's memory (`SignInDesk`); the holding window's state, over the link (mosh, encrypted); the window's memory and its opener thread's queue (wiped as it goes); the browser | the state of any other window, an action, a transcript, a log line, a driver, a state file, an argv of nexus's own, the environment |
| the code | the browser's redirect; Claude Code's listener (same machine), or the window's gate, its one action up (encrypted), the server's memory and its one replay on loopback | a transcript, a log line, a driver, any window's state |
| the token | the process's terminal; the scanner; the one-chance store and the account's `oauth-token` (0600); a late one the same way | the state, an action, a transcript, a log line, a driver's input, any window |
| the helper's nonce | the helper's environment (`NEXUS_SIGNIN_FLOW`) and its one line on the socket; the runner's memory | a log line, a state, an action; it is not a secret of the sign-in (step 3) |
| the window's claim token | the window's memory; its claim action (encrypted); the server's memory; the holding window's own state | any other window's state (it reads `*`), a log line |

Each is a `SecretValue`, which has no way to print itself and is wiped when it goes. The log names a flow by its id,
its account and its state only. The flow's AI-facing surface is one line: `signed in`, or `refused: <why>`.

**After the sign-in: the token wall.** The login the sign-in stores is the account's `oauth-token`, the same file a
paste writes, so the Claude driver starts on it as on any login: it reads the file, hands the token to Claude on a
descriptor, never in an environment, and keeps every tool its AI has out of the accounts' directory ("The token wall",
below). The sign-in's own `CLAUDE_CONFIG_DIR`, `.signin`, is inside that directory, so it is behind the wall too while
it exists, and the driver copies nothing from it into Claude's state (`drivers/claude` unit test, below).

**Platform coverage.** Whether `claude setup-token` under WSL2 calls the
helper or opens a browser by its own path is not measured; the terminal's URL is the fallback either way.

## Versions

A window, a server and a driver of different versions always work together; the asker learns in one plain answer
when the secret cannot be asked for.

| pairing | what happens |
|---|---|
| newer server, older window | The window agrees the older state format and never receives a request. With no window able to take one connected, the asker is answered `refused` (`no-client`: no client can take it), at once. |
| older server, newer window | The server sends no requests, so the window never shows a dialog or sends a secret action. |
| newer server, older driver | The driver skips the `server` line and never asks. |
| older server, newer driver | No `server` line arrives before the first `send`, so the driver knows the server takes no secrets, and its tool answers its AI `refused: no-client` without writing `ask-secret`. |
| sign-in: newer server, older window | Sign-ins are state format 5 (`docs/formats.md`), and the server reads which windows act on them from the link's format, set by each window's Hello and nothing else. A link of format 4 carries none (a window of 4 would read through the flows, the field skipped and counted, but never claim one), nor does one of 1 to 3. When only older windows that take a paste are linked, a Claude account with a configured email requires an updated window. A Claude account without an email is asked for by a paste, as before sign-ins: a needed flow ends dropped (one a window holds already goes on) and nexus asks for its `oauth-token` in the dialog, saying the window is too old to sign in in the browser. A newer window that links later shows that paste dialog too. An AI's `config sign-in` answers that the person pastes the login. |
| sign-in: older server, newer window | The link is of the server's format, 4 or older: the server sends no flows, so the window never claims, opens or binds anything, and never sends the three sign-in actions (`SignInClaim`, `SignInOpened`, `SignInCallback`). Should one be written on such a link it goes as a stop naming session 0 and nothing else, which every server refuses; a format 4 server reading the kinds themselves reads them as unknown and refuses each alone. The server asks for a paste, which the window shows. |
| sign-in: no helper | A server with no `nexus-signin-open` beside it (or whose helper socket cannot be made) says so once on its standard error, and its claude accounts are asked for by a paste, as before. |

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
the real `AccountFiles` (over the in-memory filesystem, at every write), the real `SecretLines` (the server's encoder
of each driver line and each `server.log` line) and the real `Drivers` (over an in-memory `Drivers::Host` harness, with
`Drivers::hold` and `Drivers::release` around each step) at random, and TLC checks each trace against
`models/SecretsTrace.tla` with every safety property. Each AI session's fake driver checks that `server`
(`features: ["secrets"]`) arrives on its standard input before `send`, writes `ask-secret` lines on its standard output
for `Drivers::serve` to read, and decodes every `secret` line arriving on its standard input with `driver::LineReader`
and `decodeServerMessage`. Each record of the code's state is built from the bytes each sink received: for a session
asker, solely the `secret` line decoded off its driver's standard input (and for nexus's own sign-in asks, the hub's
reply), the session lines and every entry of the store, the log line, and every diff sent to each window. Wherever any
run of 8 bytes, hex, base64 or length of a value is found in them (`tests/leak_scan.h`), the record holds that value's
model value. `SecretsTrace.tla` checks the four no-value properties on those code records too (`CodeNoSecretToAsker`,
`CodeNoSecretInTranscript`, `CodeNoSecretInLog`, `CodeNoSecretToWindows`), so a value that reaches a sink in the code
fails TLC whatever the spec allows. The faulty walks must be rejected: `--faulty-store-late` (a late answer stored
anyway) and `--faulty-driver` (a session asker's `secret` line dropped before `Drivers::tellSecret`, so its driver's
standard input never receives it) by `TraceMatches`, `--faulty-echo` by `CodeNoSecretInLog`, and `--faulty-driver-echo`
(a stored answer's `secret` line carrying the value onto the driver's standard input) by `CodeNoSecretToAsker`. The
walk also fails and heals the disk under the keeper (a write that fails, or lands unconfirmed), pastes a value not of a
token's shape one time in five, lets answers cross deadlines and restarts the server with values held, so
`ReceivedNeverDropped`, `StoredOnlyDurable` and `NoMalformedStored` are checked on the code's own traces. The echo is
injected into the code (`SecretDesk::injectEchoFault`, a test seam that puts the value into the log event of a stored
answer), not into the walk's record.

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
- `tests/server/signin_test leak` (unit): a whole sign-in with stray callbacks, an API key, a held login and a
  restart; every log line, transcript, store file and state sent (the other window's whole) searched for the URL's
  state, the code and the token.
- `tests/server/signin_functional` (functional): the runner with the FAKE `claude` under a pty, the helper, the gate
  on loopback and the replay; each case's log lines searched. `crashed`: a runner's process outlives its server and
  the next runner ends it and removes `.signin`.
- `tests/link/link_e2e signin` (functional): the real `nexus-server` with the FAKE `claude` and two windows over the
  real link: the URL in the claiming window's state alone, the flow let go of when its link ends and taken over by
  the other, the forwarded callback replayed, the token in the account's file, `config sign-in`; then every file the
  run left (`server.log` first) and every state the other window held searched. Its reversed witness,
  `link_e2e signin ... leaky`, runs `nexus-server-leaky` (a test-only build, `NEXUS_LEAKY_SIGNIN_WITNESS`, which writes
  each forwarded query to its log) and passes only when the search finds it in `server.log`.
- `drivers/claude` unit test `a login the in-app sign-in left is read...` (unit, where the sign-in meets the token
  wall): the login as the sign-in leaves it (the token's bytes alone in `oauth-token`, and a `.signin` holding the
  token in every file `claude setup-token` might write) is read by the walled driver, and nothing of `.signin` reaches
  Claude's state directory.

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
  over ssh, with the person's keys at all, only with keys of its own. A limited AI (below) has exactly that: it
  reads nothing in `~/.ssh`. An unlimited one keeps the route open.
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

### A limited AI

An AI with `"limit": true` in ais.json (docs/accounts.md, "limit"; the default for an AI added, and always for
one on an account billed at API rate) runs with the wall above and, on top of it, kept to its directory. The server
passes the driver `--limit cwd` at every start of such a session and ends, before any prompt, a driver whose hello does
not list the `limit` feature (so an older or third-party driver never runs a limited AI unlimited; a third-party
driver must enforce the option itself). The Claude driver resolves the working directory once, and that one path is
the one it checks, the one the limit allows and the one Claude works in; it adds the limit from what Claude and the
machine already have (`wall.ts` `limitOf`), nothing of nexus's own:

1. *The shell* (Claude's sandbox, as layer 2). Reads are denied in the home directory and the platform's data roots
   (macOS `/Users`, `/Volumes` and their `/System/Volumes/Data` forms; Linux and WSL2 `/home`, `/root`, `/mnt`,
   `/media`, `/run/media`, `/srv`, those that exist, and any more ais.json's `"limit_keeps_out"` names
   (docs/accounts.md; the server gives each to the driver as `--keep-out <dir>`: a second volume, a network mount, a
   WSL2 automount root moved off `/mnt`), and allowed again (`allowRead`) only for the working directory; the parts
   of the account's Claude state the shell reads (its shell snapshots, its session environment, and the project's memory folder, all inside `~/.nexus`); the programs nexus runs (node, with its
   install prefix only when that is not a denied root or a folder directly in one; the driver; the sandbox's tools);
   each PATH folder under a denied root, as that folder alone, with its parent only for an exact list of install
   prefixes (`kInstallPrefixes`: Linuxbrew's `/home/linuxbrew/.linuxbrew` on Linux; none on macOS, where a PATH folder
   such as `/Volumes/<disk>/bin` would open the whole volume); and, read only, single named files git and the shells
   read at every start (`kLimitedReadsUnderHome`: `.gitconfig`, `.config/git/config`, `.config/git/ignore`,
   `.config/git/attributes`, the bash and zsh start-up files, `.profile`, `.inputrc`; never a folder, never
   `.config/git` whole, which holds git's credential store, never a file named `credentials`, `.git-credentials`,
   `.netrc` or `_netrc`, and never `.npmrc` or `.envrc`, which hold tokens). None of the allowed paths is `~/.ssh`,
   `~/.nexus`, a folder above either, or a credential store; the Claude state folders allowed are inside `~/.nexus`, the
   account's own and nothing else of it. Writes are allowed only in the working directory and the temporary directories
   (the session's `TMPDIR` and `/tmp`, never one that holds HOME), less the files of layer 5.
2. *The file tools* (Claude's own working-directory limit): `blockReadsOutsideWorkingDirectories` and
   `blockWritesOutsideWorkingDirectories`, which hold in every permission mode and which no allow rule lifts, with the
   project's memory folder exempt for reading; and Read and Edit deny rules on `~/.ssh`.
3. *The hook* refuses a file tool any path whose every place on disk (links followed, `..` and doubled slashes resolved)
   is not in the working directory, or, to read, the project's memory folder. Deny-leaning: another spelling of the
   directory (another case on a case-insensitive volume) is refused.

A limited session does not start, in one line, when its directory does not exist, is HOME or the root, holds `~/.nexus`,
or is inside `~/.nexus` or `~/.ssh`; on a platform other than macOS and Linux; when HOME is not set; or with `--claude`
naming another Claude (whose settings nexus has not measured). It never starts unlimited instead (fail closed).

**How long an old limit lasts.** Tightening (a reload that makes a session limited) stops a turn running on a driver
started without the limit at once, through the stop's own path (the stop's deadline, then the kill); the next turn
starts limited. A weaker limit never outlasts that stop. Loosening waits for the open turn to end. models/Supervisor.tla
checks it (NeverWeaker; the witnesses that drop the stop or the retire are rejected).

**Measured** (macOS, the pinned Claude, `NEXUS_WALL_RUN=limited` and `limited-nohook` of the functional test; CI runs both
on Ubuntu and WSL2 too). With a file of another project and a made-up private key planted in the scratch HOME, outside
the working directory:

| route | unlimited | limited |
|---|---|---|
| shell: `cat` the other project's file, the private key | read | denied (1: Operation not permitted) |
| shell: `ls` HOME, `grep -r` HOME | listed, found | denied (1) |
| shell: a symbolic link in its directory to the other project, then `cat` | read | denied (1) |
| shell: write in HOME and in the other project | written | denied (1) |
| Read: the other project's file; the private key; through the link | read | refused (3, and 2 without the hook); the key also by rule |
| Grep, Glob: HOME | found | refused (3, and 2 without the hook) |
| Write: in the other project | written | refused (3, and 2 without the hook) |
| shell: a hard link it makes in its directory to the other project's file, then `cat` | read | denied (1: `ln` refused) |
| shell and Read: `..` out of its directory | read | denied (1), refused (3, and 2 without the hook) |
| shell and Read: other letter case, of a folder in HOME, of the key, and of HOME itself | read | denied (1), refused (3, 2) |
| shell and Read: another volume (a scratch root kept out by `--keep-out`, as `"limit_keeps_out"` gives it), and the same by a look-alike spelling (its accented letter decomposed) | read | denied (1), refused (3, 2) |
| shell: a file beside a PATH folder on that volume | read | denied (1); a program in the PATH folder still runs |
| shell and Read: the shell's history files | read | denied (1), refused (3, 2) |
| shell: a file in a PATH folder inside HOME; a shell start-up file | read | **read** (open, below) |
| the token, by the shell through a glob and by Read | denied (wall) | denied (wall) |
| its own directory: Write, Edit and Read a file; build and run a C program; `git status` and commit | works | works |

**What it costs.** It cannot ssh, or use git over ssh, with the person's keys (it reads nothing in `~/.ssh`); it cannot
read another project, a package cache or a toolchain kept under HOME but off the PATH (Go's module cache and build
cache, `~/.cargo`'s registry, `~/.npm`, a language version manager's installs beyond the PATH folder), nor write
caches there: point such a tool's cache into the working directory or the temporary directory (`GOCACHE`,
`GOMODCACHE`, `npm_config_cache`). On macOS a program that keeps a cache in the user's temporary folder (`cc`'s
xcrun cache) warns and runs. Claude's own state is the account's, outside the sandbox, so its memory and transcripts work.

**What stays open to a limited AI.** The limit is best effort like the wall: the AI's own user account is the
person's, and a sandboxed VM or container (`real-isolation`, the roadmap; candidates below) is the real isolation. Open:

- Everything outside the data roots and HOME, readable to its shell as to any program of the user: the system's
  folders, `/etc`, `/opt`, `/proc` on Linux and WSL2 (what it shows of other processes depends on the sandbox, not
  measured here), and whatever other users or programs leave there (macOS's `/Library`, `/Applications`,
  `/private/var` beyond its own temporary folder).
- Mounts outside the listed roots: on macOS a network share or disk image mounted somewhere other than `/Volumes`; on
  Linux an NFS or SMB mount, `/net`, `/data`, `/opt` or `/var` mounts, `/run/user/<id>` (the user's runtime folder: its
  `gvfs` mounts and sockets); on WSL2 an automount root moved off `/mnt` by `/etc/wsl.conf`, and the Windows drives
  then wherever it says. Name each in ais.json's `"limit_keeps_out"` to close it (docs/accounts.md).
- A PATH folder inside HOME or inside another project (`~/.local/bin`, `~/go/bin`, a project's `node_modules/.bin`):
  readable, as that folder alone (measured above), so whatever is kept in it.
- The shell start-up files (`.bashrc`, `.zshrc`, `.profile` and the rest above): readable, and people often export
  tokens in them. Keep tokens out of them on a machine with limited AIs.
- The account's shell-snapshot and session-environment folders: per account, shared by every AI on that account,
  unlimited ones included; what one AI's shell left there, another reads.
- The temporary folders (the session's `TMPDIR` and `/tmp`): readable **and writable**, and shared: a limited AI can
  read what another program left there, and plant a file an unlimited AI (or the person) later runs or reads.
- The network is the wall's: unchanged, every other machine reachable through the driver's proxy, so a limited AI
  still fetches, pushes over https with credentials of its own, and reaches services on other machines.
- A driver other than the Claude driver: it is given `--limit cwd` and must honour it itself; the server only requires
  it to say so (the `limit` feature in its hello).

Not a hole, a known fault: on Linux two Claude sandboxes at once, whose working directories share an ancestor a few
levels up, now and then fail each other's commands (`bwrap: Can't find source path <ancestor>/.mcp.json`; measured in
CI, 2026-11-15). The command fails closed. Only the tests avoid it (each CI job's runs go under a temporary directory
of its runner's own, one run at a time); nexus does not yet.

## Real isolation: candidates

Later, not built. The token wall and the limit are a process sandbox, best effort. Real isolation runs an AI, a
fleet worker on API billing or a CI worker inside a virtual machine or a container, so nothing it runs reaches the
machine's own files, logins or services. These are candidates, listed generously, to try and to review or reject
later; none is chosen. All but Tart are open source and
may be built in or bundled where their size allows; Tart runs as a separate program only, because its licence is
source-available, not open source, so its code is never taken into this repository. Day-to-day AIs on a Mac stay
on the process sandbox, because they need the GPU and Xcode natively.

Mac hosts, for CI runners and hostile-code work that must run on a Mac:

- Tart: virtual machines on Apple's Virtualization framework; macOS guests with Xcode, and Linux guests; directory
  sharing; cloning; a stricter network mode in which the guest cannot reach the host. A separate program only.
- Apple's containerization package and its `container` tool: a small virtual machine per container; Linux guests only;
  needs a recent macOS.
- Lima: Linux virtual machines with file sharing, without root.
- Cua's lume: a tool for macOS virtual machines.
- vfkit: a small command-line front end to Apple's virtualization; a building block rather than a whole answer.
- Podman's virtual machine mode, which runs its rootless containers on a Mac (and on Windows).

Linux and WSL2 hosts:

- libkrun through an embeddable microVM library such as BoxLite or microsandbox: one interface on Linux, macOS and
  WSL2, and can be built in.
- Firecracker with a runner-per-VM manager such as Fireactions, for CI runners: the most proven; Linux only; runs as a
  root service.
- Cloud Hypervisor: a microVM per run on the machine's own hypervisor (KVM), with a minimal guest kernel and warm
  snapshots; the base of the per-run microVM idea already on the ideas list.
- gVisor: a kernel in user space, so no virtual machine is needed; weaker network control without root.
- Kata Containers: a virtual machine per container, under Kubernetes.
- Podman: rootless containers.
- Incus: system containers and virtual machines.

Any host, on our own machines or on GCP or AWS:

- Kubernetes itself, running AIs, workers and CI in isolated pods or virtual machines; the agent-sandbox resource
  type and a runner controller are the pieces to look at.
- Virtual machines on GCP or AWS, with AIs and workers started there.

The process-level baseline, for comparison: bubblewrap, which Claude's sandbox already uses on Linux and WSL2 (with
Seatbelt on macOS), and nsjail, a comparable process jail.
