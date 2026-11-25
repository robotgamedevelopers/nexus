# The driver protocol, version 1

nexus-server reaches each AI through a driver: a small program that sits between the server and one AI harness
(Claude, Codex and others). The server starts one driver process per session (a conversation with an AI, which
the server identifies by an id of its own) and talks to it over the driver's standard input and output. This page
is the whole contract a driver must keep. The reference code is `src/driver/protocol.h`;
`tests/driver/fake_driver.cpp` is a driver that plays a script.

## Starting and ending

The server starts the driver with these arguments:

| argument | meaning |
|---|---|
| `--cwd <dir>` | the directory the harness works in; always given |
| `--resume <session id>` | resume this harness session (an id the driver reported in a `session` message); absent for a new session |
| `--limit cwd` | the AI is limited to its directory (`docs/accounts.md`, "limit"): the AI reads and writes only in `--cwd` (and what its harness needs to run); absent for an AI not limited. A driver that supports it lists `"limit"` in its hello's `features`; the server never relies on an unknown argument being refused, and ends a driver that does not list it. A driver that lists it but cannot enforce it here refuses to start with an `error` (code `other`), never runs unlimited |
| `--keep-out <dir>` | with `--limit cwd` only, once per directory of ais.json's `"limit_keeps_out"` (`docs/accounts.md`, "limit"): one more absolute directory the limited AI is kept out of, on top of the ones the driver always keeps out. It only adds. A driver refuses it without `--limit cwd`, and refuses a directory that is not absolute or is `/`; an older driver refuses the argument it does not know, so it never runs weaker |

When started through the [AI launcher](policy.md#ai-identity-at-startup), the driver also receives
`--startup-context-fd N`. It reads the UTF-8 standing instructions from this inherited descriptor, closes it,
and supplies them to every harness creation, including resume and recovery from a missing session ID.
A driver that cannot apply the context must refuse startup. The person's protocol messages retain their original text.

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
| `steer` | `turn` (required), `id` (whole number from 1, required), `text` (string, required) | a message from the person into the open turn `turn`, taken in after the tool calls running, before the next model call; written only to a driver whose hello names `"steer"` (below, "Steering a turn") |
| `follow-up` | `turn` (required), `id` (whole number from 1, required), `text` (string, required) | a message from the person into the open turn `turn`, taken in only when the turn would otherwise settle; written only to a driver whose hello names `"follow-up"` (below) |
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
| `hello` | `protocol` (whole number, required: 1), `harness` (non-empty string, required, as `"claude"`), `features` (array of strings, optional) | the first line, always. `features` lists what the driver supports beyond version 1: `"limit"` means it keeps the AI to its directory under `--limit cwd`. The server ends a driver started with `--limit cwd` whose hello does not list `"limit"`, before any `send`, and tells the session why. `"steer"`, `"follow-up"` and `"settled"` are steering (below); a driver that names `"steer"` or `"follow-up"` names `"settled"` too, and the server ends one that does not. A feature the server does not know is skipped |
| `credential` | `billing` (required: `"subscription"`, `"api-key"` or `"none"`), `plan` (string, optional: the plan's name as the harness reports it) | which credential the harness is using |
| `session` | `id` (required: 1 to 256 bytes of printable ASCII, no spaces, not starting with `-`) | the harness's session id: what the server passes as `--resume` when it starts the driver again |
| `text` | `turn`, `delta` (string, required) | a piece of the turn's current text block, appended to it |
| `text-end` | `turn` | the turn's current text block is complete |
| `tool` | `turn`, `name` (non-empty string, required), `summary` (string, required) | the AI started using a tool; the summary says what it does, in one line |
| `done` | `turn`, `outcome` (required: `"ok"`, `"stopped"` or `"error"`), `session_cost_usd` (finite number, zero or more, optional), `usage` (object, optional) | the turn has ended |
| `error` | `cause` (non-empty string, required), `fix` (non-empty string, required: what a person does about it), `turn` (optional), `code` (string, optional) | something went wrong |
| `queue` | `turn` (required), `steer` (array of ids, required), `follow_up` (array of ids, required) | the driver's whole queue for turn `turn`: the ids of the steers and of the follow-ups it took and has not yet given its harness, oldest first; written after every change of either list, never a diff (below) |
| `settled` | `turn` (required), `aborted` (`true` or `false`, required) | turn `turn` has no automatic work left; written once, just before its `done`, by a driver whose hello names `"settled"`; `aborted` when a stop ended it (below) |
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
| `usage-limit` | the account exhausted its included subscription usage. The server applies the session’s account-selection policy. |
| `model-limit` | Claude reported a Fable-specific limit on this turn. The server can offer the person a choice of account or model. |
| `other` | anything else |

Every message that belongs to a turn carries that turn's number, so the server can drop messages from a turn it no
longer waits for (after a restart of either side).

## Steering a turn

A person may write into a turn while it runs, as in pi's RPC mode (pi.dev, MIT), whose steer, follow-up, queue
update and settled events these follow: a **steer** says "change course now", a **follow-up** says "and after that,
this". Each driver says in its hello which it can do with its own harness; the server writes a kind only to a driver
that named it, and never relies on a driver skipping it. A driver that names neither keeps working exactly as before:
the person's message waits as the next prompt.

- **Ids.** The server numbers each steer and follow-up it writes with an `id` of its own, from 1, strictly increasing
  within the driver's process (across turns). A queue names messages by these ids, never by their text, so that a
  queue line stays small.
- **Turn numbers.** A steer or follow-up carries the number of the open turn it goes into, and consumes no turn
  number: what the harness does with it is part of that turn, and every message it writes carries that turn's number.
- **When it goes in.** A steer goes in at the next tool-call boundary (the tool calls the model asked for are done,
  before its next call), or at the end of a model run: every steer pending goes in together. A follow-up goes in only
  at the end of a model run with no tool calls and no steer pending, one per model run, oldest first. Each goes to the
  harness at most once.
- **The queue.** The driver writes `queue` after every change of its queue, with both lists whole: when it takes a
  steer or follow-up, and when its harness takes one in. An id leaves the queue only when the harness takes it in, so
  the server knows from the reports which were taken in. Each list is oldest first, so its ids go up; at most 64 a
  list (a server writes no more than 64 into one turn).
- **Settled.** The end of one model run is not the end of the turn while a retry, a compaction, a steer or a
  follow-up can follow it. A driver that names `"settled"` writes `settled` once a turn has nothing pending and nothing
  automatic left, and then the turn's `done`; nothing of the turn comes between them, and no `queue` after `settled`.
  A `stop` of the open turn settles it at once, `aborted`, with its queue left as it was: what that queue held was never
  given to the harness. A driver that names it writes `settled` before every `done`, steered or not.
- **`settled` and `done`.** `done` stays the last message of its turn and the one that closes it: sequence rule 3 is
  unchanged, and the server writes the next `send` only after the open turn's `done`. `settled` is the driver's
  statement that the turn's queue is final, so a server that reads `done` without `settled` from a driver that named
  it ends the driver.
- **Crossing the end.** The server writes a steer or follow-up only while the turn is open and it has read neither
  `settled` nor `done` for it, and not after a stop. One can still cross the turn's end on the way: a driver ignores a
  steer or follow-up for a turn that is not open, or that it has settled, and answers it with nothing, as it does a
  stray `stop`. At the turn's `done` the server takes every message it wrote into the turn that the driver never took
  in (never queued, or still in the last queue) and puts it first among the waiting prompts, in the order written:
  each is the next turn's prompt, so nothing the person wrote is lost while the driver lives. If the driver exits with
  the turn open, what it had not taken in is cut with the turn, with a notice, and never written again.
- **Refused.** A steer or follow-up to a driver whose hello did not name the kind, for a turn the driver never opened
  (above the last `send`), or whose `id` is not above the last, is the server's bug: the driver refuses the line (rule
  4), carrying its `turn`. A `queue` or `settled` from a driver that did not name it, for a turn that is not open, a
  `queue` naming an id the server did not write into the turn as that kind or one the harness already took in, a
  second `settled`, anything of the turn but its `done` after `settled`, and a `done` before `settled` are the
  driver's: the server ends it.

These came into version 1 as kinds an older reader skips (rule 8), offered only to a driver that names them, so the
version stays 1: an older driver never names them and is never written them, and an older server skips the `queue`
and `settled` of a newer driver, whose `done` still closes each turn. `models/Steering.tla` is the model, bound to the
server's supervisor and to the driver's side (`src/driver/steering.h`, which the fake driver plays) by
`tests/driver/steering_walk.cpp`.

## Sequence rules

1. The driver's first line is `hello`. The server refuses a driver whose first line is anything else, or whose
   `protocol` is not one it speaks. A driver that cannot work (a bad argument, no usable credential) still writes
   `hello` first, then an `error`, then exits.
2. After `hello`, the driver may write `credential` and `session` at any time, whether or not a turn is open, and any
   number of times; the latest `credential` and the latest `session` are the ones that hold. A harness may know its
   session id and credential only once its first turn has started.
3. At most one turn is open at a time. A `send` opens its turn; the turn's `done` closes it. The server writes the next
   `send` only after the open turn's `done` (from a driver that names `"settled"`, its `settled` comes first: a turn is
   never closed at the end of one model run while more can follow). A `steer` or `follow-up` goes into the open turn
   and opens none.
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
< {"kind":"hello","protocol":1,"harness":"claude","features":["limit"]}
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

A turn steered while it runs, by a driver whose hello named all three: the steer goes in after the tool call, the
follow-up when the run would otherwise end, then settled and done. A second follow-up, written as the driver settles the turn,
crosses its end: the driver had already written settled when it read it, so it ignores it, and at the done the server
sends it as the next turn's prompt:

```
< {"kind":"hello","protocol":1,"harness":"fake","features":["steer","follow-up","settled"]}
> {"kind":"send","turn":4,"prompt":"Fix the failing test."}
< {"kind":"tool","turn":4,"name":"Bash","summary":"ctest -R parser"}
> {"kind":"steer","turn":4,"id":1,"text":"Only the parser, leave the lexer alone."}
< {"kind":"queue","turn":4,"steer":[1],"follow_up":[]}
> {"kind":"follow-up","turn":4,"id":2,"text":"Then run the whole suite."}
< {"kind":"queue","turn":4,"steer":[1],"follow_up":[2]}
< {"kind":"queue","turn":4,"steer":[],"follow_up":[2]}
< {"kind":"text","turn":4,"delta":"Fixed in the parser only."}
< {"kind":"text-end","turn":4}
< {"kind":"queue","turn":4,"steer":[],"follow_up":[]}
< {"kind":"tool","turn":4,"name":"Bash","summary":"ctest"}
< {"kind":"text","turn":4,"delta":"All pass."}
< {"kind":"text-end","turn":4}
> {"kind":"follow-up","turn":4,"id":3,"text":"And commit."}
< {"kind":"settled","turn":4,"aborted":false}
< {"kind":"done","turn":4,"outcome":"ok"}
> {"kind":"send","turn":5,"prompt":"And commit."}
```

A driver can emit `{"kind":"child-agents","count":2}` after hello to publish its current number of active child agents. This process-scoped snapshot excludes the parent. Send zero when the last child ends. The supervisor resets the count on driver exit.
