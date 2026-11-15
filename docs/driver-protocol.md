# The driver protocol, version 1

nexus-server reaches each friend through a driver: a small program that sits between the server and one AI harness
(Claude, Codex and others). The server starts one driver process per session (a conversation with a friend, which
the server identifies by an id of its own) and talks to it over the driver's standard input and output. This page
is the whole contract a driver must keep. The reference code is `src/driver/protocol.h`;
`tests/driver/fake_driver.cpp` is a driver that plays a script.

## Starting and ending

The server starts the driver with these arguments:

| argument | meaning |
|---|---|
| `--cwd <dir>` | the directory the harness works in; always given |
| `--resume <session id>` | resume this harness session (an id the driver reported in a `session` message); absent for a new session |
| `--limit cwd` | the friend is limited to its directory (`docs/accounts.md`, "limit"): the AI reads and writes only in `--cwd` (and what its harness needs to run); absent for a friend not limited. A driver that cannot enforce it, or does not know the argument, refuses to start with an `error` (code `other`), never runs unlimited |

The account the harness uses is chosen by the environment the server starts the driver with, never by the protocol.
The driver's standard error is free text for logs; the server shows none of it as conversation.

When its standard input ends, a driver stops the harness (and any turn it is running) and exits promptly, without
owing a `done` for an open turn. How the server ends a driver is the server's own business: it closes the driver's
standard input first, and ends a driver that does not exit.

## Lines

- Each message is one JSON object on one line, UTF-8, ended by a newline (`\n`), in both directions. A line may end in
  CR LF; the CR is dropped. A line may not start with a byte-order mark.
- A driver flushes its output after every line, so each message reaches the server as it is written.
- A line is at most 1,048,576 bytes (1 MiB), not counting its newline (a CR before the newline counts). That holds any
  prompt a person types or pastes, while a driver's replies come in small pieces. A reader refuses a longer line as
  soon as it passes the limit, drops the rest of it up to its newline, and goes on with the next line; it never holds
  more than the limit of one line. A writer never writes one: a prompt too long is refused before it is sent.
- Every object has a `"kind"`, a non-empty string. The other fields depend on the kind (below).
- A field this version does not define is ignored. An object whose kind this version does not define is skipped. This
  is how later versions add to the protocol.
- An optional field may be absent or `null`; the two mean the same.
- Of duplicate keys in one object, the last wins (a later `null` makes the field absent).
- A malformed line is refused with its cause, and reading goes on with the next line: one that is not UTF-8 (including
  an encoded surrogate, an overlong form, or a `\u` escape of a lone surrogate such as `"\ud83d"`), not JSON, nested
  more than 1024 arrays and objects deep, not a single object, without a `"kind"`, or of a known kind with a required
  field missing or of the wrong type or value. The server refuses a driver's line and goes on, or not, as it
  chooses. A driver that refuses a line from the server ends (sequence rule 4).
- A **whole number** is a JSON number whose value, read as a double, has no fraction: `1`, `1.0` and `1e0` are all 1;
  `1.5` is refused. A **turn** is a whole number from 1 to 2^53 - 1, the largest a double holds exactly; so is a token
  count, from 0.
- Strings hold whole Unicode characters. A `text` delta never splits a character between two messages.

## Server to driver

| kind | fields | meaning |
|---|---|---|
| `send` | `turn` (required), `prompt` (string, required) | a prompt; opens turn `turn` |
| `stop` | `turn` (required) | stop turn `turn`, if it is the open turn |
| `settings` | `model` (non-empty string, optional), `effort` (non-empty string, optional); at least one | the model and effort for the turns that follow |
| `server` | `features` (array of strings, required) | what this server takes beyond version 1, written once, right after the driver's `hello` and before any `send`: `"secrets"` means it takes `ask-secret` (docs/secrets.md). A driver that does not know the kind skips it (rule 8) and never asks. |
| `secret` | `ask` (whole number from 1, required), `outcome` (required: `"stored"` or `"refused"`), `reason` (non-empty string, required with `refused`: `"busy"`, `"no-client"`, `"cancelled"`, `"timeout"` or `"malformed"`; servers before the one-chance rule also sent `"unwritable"`, and `"unconfirmed"` with `stored`, and a driver still reads them) | the one answer to the driver's `ask-secret` numbered `ask`; never the value. `stored` is written only once the value is in its file and the file is durable: a value whose file cannot be written yet is held by the server and written again until it is (docs/secrets.md, "One chance"), so the answer may come long after the paste. `malformed` means what was pasted was not one token (the person is told why); the AI may ask again. |

`settings` came into version 1 after it was first written. A driver that does not know it skips it, as any unknown kind
(sequence rule 8), so the version stays 1; a server that needs to know whether a driver honours it asks of the driver's
harness, not of the version. Its values are the harness's own names (for Claude a model id or alias, and an effort of
`low`, `medium`, `high`, `xhigh` or `max`). A field left out stays as it was. It may come at any time after the driver
starts, a turn open or not, and applies from the next `send` on: a turn never changes model or effort halfway. A value
the harness does not take is answered by an `error` without a turn (code `other` for a value it does not have,
`harness` when the harness refuses it), and the setting stays as it was.

The server numbers turns from 1 for each session, strictly increasing and never reused, across restarts of the driver: a
driver that has just started may see its first `send` with any turn number. A driver checks the order: each `send`'s
turn must be greater than the last turn that driver process saw.

The server writes each prompt to a driver at most once and never sends it again. If a driver exits while a turn is
open, the server ends that turn itself and marks it as cut short: whether the harness took the prompt, or finished its
reply, is unknown, and the person decides whether to send it again.

## Driver to server

| kind | fields | meaning |
|---|---|---|
| `hello` | `protocol` (whole number, required: 1), `harness` (non-empty string, required, as `"claude"`) | the first line, always |
| `credential` | `billing` (required: `"subscription"`, `"api-key"` or `"none"`), `plan` (string, optional: the plan's name as the harness reports it) | which credential the harness is using |
| `session` | `id` (required: 1 to 256 bytes of printable ASCII, no spaces, not starting with `-`) | the harness's session id: what the server passes as `--resume` when it starts the driver again |
| `text` | `turn`, `delta` (string, required) | a piece of the turn's current text block, appended to it |
| `text-end` | `turn` | the turn's current text block is complete |
| `tool` | `turn`, `name` (non-empty string, required), `summary` (string, required) | the friend started using a tool; the summary says what it does, in one line |
| `done` | `turn`, `outcome` (required: `"ok"`, `"stopped"` or `"error"`), `session_cost_usd` (finite number, zero or more, optional), `usage` (object, optional) | the turn has ended |
| `error` | `cause` (non-empty string, required), `fix` (non-empty string, required: what a person does about it), `turn` (optional), `code` (string, optional) | something went wrong |
| `ask-secret` | `ask` (whole number from 1, required: the driver's own number, unique within its process), `account` (non-empty string, required), `what` (non-empty string, required: a file the account's provider documents, as `"oauth-token"`), `why` (string, required: one line, no control characters, at most 200 bytes) | ask the person, in nexus's window, for a secret for the account (docs/secrets.md); written only after a `server` line naming `"secrets"`. The server answers with one `secret` line; an ask it cannot take (an account not set up, a file not documented, a `why` past its limit) is refused as a malformed line: no request is made and, as for any line the server refuses, the driver is ended. |

`turn` is required wherever the table names it without saying optional.

A tool's `name` and `summary` hold no control characters (U+0000 to U+001F, U+007F to U+009F, which take in tab, CR,
LF and U+0085) and no line or paragraph separator (U+2028, U+2029). A `tool` is written when the tool starts.

`session_cost_usd` is the harness's own running estimate of what the whole session has cost so far, in US dollars,
not the cost of this turn; a harness that gives none leaves it out. `usage` holds token counts the harness reported
with the turn's result, each an optional whole number: `input_tokens`, `output_tokens`, `cache_read_tokens`,
`cache_write_tokens`.

An error's `code` is one of these; an absent code, or one this version does not define, reads as `other`:

| code | meaning |
|---|---|
| `protocol` | a rule of this protocol was broken (by the server, when a driver says so) |
| `no-credential` | the harness has no credential it can use; the driver ends the open turn, if any, with this error (with the turn) and `done` outcome `error`, then exits |
| `session-not-found` | the harness does not know the `--resume` id, at start or once the first turn begins; a notice, after which the driver carries on in a new session (sequence rule 10) |
| `harness` | the harness itself failed |
| `usage-limit` | the account reached its usage limit; nexus-server moves the session to its friend's next signed-in account (docs/accounts.md), or tells it there is none |
| `other` | anything else |

Every message that belongs to a turn carries that turn's number, so the server can drop messages from a turn it no
longer waits for (after a restart of either side).

## Sequence rules

1. The driver's first line is `hello`. The server refuses a driver whose first line is anything else, or whose
   `protocol` is not one it speaks. A driver that cannot work (a bad argument, no usable credential) still writes
   `hello` first, then an `error`, then exits.
2. After `hello`, the driver may write `credential` and `session` at any time, whether or not a turn is open, and any
   number of times; the latest `credential` and the latest `session` are the ones that hold. A harness may know its
   session id and credential only once its first turn has started.
3. At most one turn is open at a time. A `send` opens its turn; the turn's `done` closes it. The server writes the next
   `send` only after the open turn's `done`.
4. A line from the server that the driver cannot accept is the server's bug, whatever it is: a line the reader refuses
   (malformed, or a known kind with a field missing or wrong, as `{"kind":"send","turn":7}`), or a `send` while a turn
   is open or whose turn is not greater than the last turn this driver process saw. The driver writes one `error` with
   code `protocol`, carrying the line's `turn` if the line has one it can read, then exits with a status other than 0.
   It never leaves the server waiting.
5. Within a turn the driver writes, in any number and order: text blocks (one or more `text`, then `text-end`) and
   `tool`. A text block is ended by its `text-end` before a `tool`, another block or `done`. The turn ends with exactly
   one `done`, the last message of its turn: nothing with that turn's number follows it.
6. An `error` with a `turn` is written only while that turn is open, except the one that refuses a line (rule 4). An
   `error` without a turn may come at any time after `hello`. A turn that ends with `done` outcome `error` has at least
   one `error` with that turn before its `done`, giving the cause (a `session-not-found` notice does not count).
7. A `stop` whose turn is the open turn ends that turn: the driver delivers the text the harness had already produced
   (and its `text-end`, if a text block is open), then `done` with outcome `stopped`. If no text block is open, the
   driver writes just the `done`. Any other `stop` (for a turn that is not open, or with no turn open) is ignored and
   answered by nothing. So a `stop` that crosses a turn's `done` on the way, or that is meant for a turn already ended,
   does nothing.
8. A message of an unknown kind is skipped silently, by either side.
9. When its standard input ends, the driver stops the harness and exits, owing no `done` for an open turn (above). If
   the driver exits for any reason, the open turn gets no `done` from it; the server ends that turn itself and marks it
   as cut short.
10. A driver started with `--resume` whose id the harness does not know (found at start, or only once the first turn
    begins) neither exits nor loses the prompt. It starts a new harness session itself, writes an `error` with code
    `session-not-found` (with the open turn if there is one), writes the new session's `session`, and the turn carries
    on in the new session. That error is a notice: it does not by itself make the turn's outcome `error`.
11. A driver with no credential it can use (code `no-credential`) ends the open turn, if any, with that `error`
    (carrying the turn) and `done` outcome `error`, then exits.

A driver holds no state the server cannot rebuild: the server keeps the turn numbers and the last session id, and
starts a new driver with `--resume` to go on.

## A worked exchange

Lines prefixed `>` go from the server to the driver, `<` from the driver to the server. The driver starts:

```
< {"kind":"hello","protocol":1,"harness":"claude"}
```

A turn, with a text block in two pieces, a tool, and a second block:

```
> {"kind":"send","turn":1,"prompt":"What is in src?"}
< {"kind":"credential","billing":"subscription","plan":"max"}
< {"kind":"session","id":"5f0c2a8e-1d34-4b7e-9a51-3c6f2b0d9e77"}
< {"kind":"text","turn":1,"delta":"Let me "}
< {"kind":"text","turn":1,"delta":"look."}
< {"kind":"text-end","turn":1}
< {"kind":"tool","turn":1,"name":"Bash","summary":"ls src"}
< {"kind":"text","turn":1,"delta":"It holds the server and the window."}
< {"kind":"text-end","turn":1}
< {"kind":"done","turn":1,"outcome":"ok","session_cost_usd":0.021,"usage":{"input_tokens":1840,"output_tokens":96}}
```

A turn stopped part way, then a stop for the turn already ended, which gets no answer:

```
> {"kind":"send","turn":2,"prompt":"Now read every file."}
< {"kind":"text","turn":2,"delta":"Reading the first"}
> {"kind":"stop","turn":2}
< {"kind":"text","turn":2,"delta":" one"}
< {"kind":"text-end","turn":2}
< {"kind":"done","turn":2,"outcome":"stopped","session_cost_usd":0.034}
> {"kind":"stop","turn":2}
```

A change of model and effort, between turns, which applies from the next one:

```
> {"kind":"settings","model":"claude-sonnet-5-5","effort":"high"}
```

A turn the harness fails, with its cause first:

```
> {"kind":"send","turn":3,"prompt":"Go on."}
< {"kind":"error","cause":"the harness lost its connection","fix":"send the prompt again","turn":3,"code":"harness"}
< {"kind":"done","turn":3,"outcome":"error"}
```
