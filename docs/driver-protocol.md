# The driver protocol, version 1

nexus-server reaches each friend through a driver: a small program that sits between the server and one AI harness
(Claude, Codex and others). The server starts one driver process per friend tab and talks to it over the driver's
standard input and output. This page is the whole contract a driver must keep. The reference code is
`src/driver/protocol.h`; `tests/driver/fake_driver.cpp` is a driver that plays a script.

## Starting

The server starts the driver with these arguments:

| argument | meaning |
|---|---|
| `--cwd <dir>` | the directory the harness works in; always given |
| `--resume <session id>` | resume this harness session (an id the driver reported in a `session` message); absent for a new session |

The account the harness uses is chosen by the environment the server starts the driver with, never by the protocol.
The driver's standard error is free text for logs; the server shows none of it as conversation.

## Lines

- Each message is one JSON object on one line, UTF-8, ended by a newline (`\n`), in both directions.
- A line is at most 1,048,576 bytes (1 MiB), not counting its newline. That holds any prompt a person types or pastes,
  while a driver's replies come in small pieces. A reader refuses a longer line as soon as it passes the limit, drops
  the rest of it up to its newline, and goes on with the next line; it never holds more than the limit of one line. A
  writer never writes one: a prompt too long is refused before it is sent.
- Every object has a `"kind"`, a non-empty string. The other fields depend on the kind (below).
- A field this version does not define is ignored. An object whose kind this version does not define is skipped. This
  is how later versions add to the protocol.
- An optional field may be absent or `null`; the two mean the same.
- A malformed line is refused with its cause, and reading goes on with the next line: one that is not UTF-8 (including
  an encoded surrogate or an overlong form), not JSON, not a single object, without a `"kind"`, or of a known kind with
  a required field missing or of the wrong type or value. What a side does about a refused line is its own choice; a
  driver reports it with an `error`.
- A **turn** is an integer from 1 to 2^53 - 1 (the largest a JSON number holds exactly). An **integer** field never has a
  fraction.

## Server to driver

| kind | fields | meaning |
|---|---|---|
| `send` | `turn` (integer, required), `prompt` (string, required) | a prompt; opens turn `turn` |
| `stop` | none | stop the open turn |

The server numbers turns from 1 for each tab, strictly increasing and never reused, across restarts of the driver: a
driver that has just started may see its first `send` with any turn number.

## Driver to server

| kind | fields | meaning |
|---|---|---|
| `hello` | `protocol` (integer, required: 1), `harness` (non-empty string, required, as `"claude"`) | the first line, always |
| `credential` | `billing` (required: `"subscription"`, `"api-key"` or `"none"`), `plan` (string, optional: the plan's name as the harness reports it) | which credential the harness is using |
| `session` | `id` (non-empty string, required) | the harness's session id: what the server passes as `--resume` when it starts the driver again |
| `text` | `turn`, `delta` (string, required) | a piece of the turn's current text block, appended to it |
| `text-end` | `turn` | the turn's current text block is complete |
| `tool` | `turn`, `name` (non-empty string, required), `summary` (string with no line break, required) | the friend used a tool; the summary says what it did, in one line |
| `done` | `turn`, `outcome` (required: `"ok"`, `"stopped"` or `"error"`), `session_cost_usd` (number, zero or more, optional), `usage` (object, optional) | the turn has ended |
| `error` | `cause` (non-empty string, required), `fix` (non-empty string, required: what a person does about it), `turn` (optional) | something went wrong |

`turn` is required wherever the table names it without saying optional.

`session_cost_usd` is the harness's own running estimate of what the whole session has cost so far, in US dollars,
not the cost of this turn; a harness that gives none leaves it out. `usage` holds token counts the harness reported
with the turn's result, each an optional integer of zero or more: `input_tokens`, `output_tokens`, `cache_read_tokens`,
`cache_write_tokens`.

Every message that belongs to a turn carries that turn's number, so the server can drop messages from a turn it no
longer waits for (after a restart of either side).

## Sequence rules

1. The driver's first line is `hello`. The server refuses a driver whose first line is anything else, or whose
   `protocol` is not one it speaks. A driver that cannot work (a bad argument, no usable credential) still writes
   `hello` first, then an `error`, then exits.
2. After `hello`, the driver may write `credential`, `session` and `error` at any time, whether or not a turn is open,
   and any number of times; the latest `credential` and the latest `session` are the ones that hold. A harness may know
   its session id and credential only once its first turn has started.
3. At most one turn is open at a time. A `send` opens its turn; the turn's `done` closes it. The server writes the next
   `send` only after the open turn's `done`. A `send` while a turn is open is a protocol error by the server; the driver
   reports it with an `error` and drops that `send`.
4. Within a turn the driver writes, in any number and order: text blocks (one or more `text`, then `text-end`) and
   `tool`. A text block is ended by its `text-end` before a `tool`, another block or `done`. The turn ends with exactly
   one `done`, the last message of its turn: nothing with that turn's number follows it.
5. `stop` while a turn is open ends that turn: the driver delivers the text the harness had already produced (then its
   `text-end`), then `done` with outcome `stopped`. If the turn had already finished before the driver read the `stop`,
   its `done` (with whatever outcome it had) has been or is being written, and the `stop` is then one with no open turn.
6. `stop` with no open turn is ignored and answered by nothing.
7. A message the reader refuses is reported and skipped (above); a message of an unknown kind is skipped silently.
8. When its standard input ends, the driver ends its harness session and exits. If the driver exits, the open turn
   gets no `done` from it; the server ends that turn itself.

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

A turn stopped part way, then a stop with no turn open, which gets no answer:

```
> {"kind":"send","turn":2,"prompt":"Now read every file."}
< {"kind":"text","turn":2,"delta":"Reading the first"}
> {"kind":"stop"}
< {"kind":"text","turn":2,"delta":" one"}
< {"kind":"text-end","turn":2}
< {"kind":"done","turn":2,"outcome":"stopped","session_cost_usd":0.034}
> {"kind":"stop"}
```
