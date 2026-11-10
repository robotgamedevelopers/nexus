# models: the TLA+ specifications of nexus

Every state machine in nexus has a TLA+ module here. TLC checks each module on a small instance, with reversed witnesses, and the code is bound to the module: the code's own random walk writes traces, and TLC checks that each trace is a behaviour of the module.

## What is modelled

`Window.tla` is the state the window owns, `nexus::Model` in `src/model.h`:

- `friends`: one tab per friend, fixed when the model is built;
- `active`: the active tab, 0 when there are no tabs;
- `transcript`: each tab's entries, kinds `Person`, `Queued`, `Friend`, `Notice`;
- `draft`: each tab's unsent text.

Tabs are indexed from 0, as in the code. The actions are the model's operations, with the arguments a caller passes, in range and out of it: `SelectTab(i)`, `SetDraft(i, t)`, `Send(i)`, `Queue(i)`, `AppendEntry(i, e)`. A refused operation changes no model state.

`last` is a history variable, not model state: the operation just performed, its arguments and the value it returned. The action properties are stated through it, and a code trace's return values are checked against it.

All-whitespace means every byte is an ASCII space, tab, newline, vertical tab, form feed or carriage return; the empty draft is all whitespace. The spec does not compute this: the constant `Blank` names the drafts that are all whitespace, and each instance lists them.

### Properties

| Property | Kind | What it says |
|---|---|---|
| `TypeOK` | invariant | every variable has its type; the friends are the ones the model was built with |
| `ActiveValid` | invariant | `active` is a tab, or 0 when there are none |
| `SelectExact` | action | select returns true exactly for a tab, and changes only `active`, to that tab |
| `SetDraftExact` | action | set_draft returns true exactly for a tab, and changes only that tab's draft, to the text given |
| `SendResult` | action | send and queue return true exactly for a tab whose draft is not all whitespace |
| `SendAppendsOne` | action | a send or queue that returns true appends one entry, `Person` or `Queued`, with the draft's text, to that tab |
| `SendClearsOnlyItsDraft` | action | it clears that tab's draft and no other |
| `SendTouchesNoOtherTab` | action | it changes no other tab's transcript or draft, nor `active`, nor the friends |
| `AppendExact` | action | append returns true exactly for a tab, appends exactly that entry to that tab, and changes nothing else |
| `RefusedChangesNothing` | action | an operation that returns false changes no model state |
| `TranscriptsOnlyGrow` | action | entries are only appended: existing entries never change, move or go |
| `FriendsFixed` | action | the friends never change |

### Instances

`MCWindow.tla` gives the constants values and bounds transcript length (`MaxLen`, a state constraint). The drafts are `""`, `" "`, `" \t\n"` and `"hi"`; the appended entries are a friend's reply, an empty notice and an all-whitespace `Person` entry (append takes any entry). Indices run to one past the last tab. TLC tells states apart by the model variables alone (`VIEW ModelView`), which is sound because no action reads `last` and no property reads it unprimed.

| Config | Instance |
|---|---|
| `MCWindow.cfg` | two tabs, transcripts up to 2 entries |
| `MCWindow3.cfg` | three tabs, transcripts up to 1 entry |
| `MCWindowEmpty.cfg` | no friends, so no tabs: every operation is refused |

## Reversed witnesses

Each module in `witnesses/` extends `MCWindow` and passes one broken action to `NextWith` in place of the real one. Its config checks only the property that must catch it, and the test passes only when TLC reports that property violated. A witness that passes is a failure of the check.

| Witness | The break | Caught by |
|---|---|---|
| `WSendClearsAll` | a send clears every tab's draft | `SendClearsOnlyItsDraft` |
| `WSendToActive` | send appends to the active tab instead of the named one | `SendTouchesNoOtherTab` |
| `WSelectOutOfRange` | select out of range returns false but moves `active` | `ActiveValid` |
| `WSendWhitespace` | send and queue accept an all-whitespace draft | `SendResult` |
| `WRewriteEntry` | append also overwrites the first entry | `TranscriptsOnlyGrow` |
| `WRefusedClearsDraft` | a send refused for whitespace clears the draft | `RefusedChangesNothing` |
| `WQueueAsPerson` | queue appends kind `Person` | `SendAppendsOne` |
| `WSetDraftActiveToo` | set_draft writes the active tab's draft too | `SetDraftExact` |
| `WSelectClearsDraft` | select clears the draft of the tab it leaves | `SelectExact` |
| `WAppendWrongTab` | append puts the entry on the next tab | `AppendExact` |

## The binding: code traces checked against the spec

`tests/model/trace_walk.cpp` drives the real `nexus::Model` with a seeded random walk (`trace_walk <seed> <steps> <friends> [--faulty] <out.tla>`). The random numbers come from the seed alone. Each step calls one operation, with indices in range and two past the last tab, drafts that are empty, all whitespace (including tab, newline, carriage return and form feed), or text with quotes and backslashes, and entries of every kind. The walk writes `WindowTraceData.tla`: the arguments' domains and `Trace`, one record per step, holding the operation, its arguments, the return value and the model's whole state after it.

`WindowTrace.tla` instantiates `Window` with those domains. Each of its steps takes the `Window` action the record names, with the record's arguments, and the invariant `TraceMatches` requires the spec's state and return value to equal the code's. A step whose arguments lie outside the spec's domains has no successor and TLC reports a deadlock. The property `WindowSpec` checks the whole behaviour against `Window`'s `Init` and `Next`, and every `Window` property is checked on the trace too.

The binding is shown to fail: `--faulty` walks a test-only subclass whose `send` accepts a draft that is all whitespace but not empty. TLC must report `TraceMatches` violated at the first such send.

## Running the checks

TLC needs Java 11 or newer and `tla2tools.jar`. The tests find the jar through `TLA2TOOLS_JAR`, and java through `JAVA` or `PATH`. Without `TLA2TOOLS_JAR` every `model` test fails and says what to set.

```sh
cmake -S . -B build
cmake --build build --target model_checks -j4
ctest --test-dir build -L unit
TLA2TOOLS_JAR=/path/to/tla2tools.jar ctest --test-dir build -L model -j4
```

The `model` label runs the three instances, the ten witnesses, seven code traces (seeds 1 to 5 on three tabs and 300 steps, seed 6 on two tabs, seed 7 with no tabs) and the faulty trace. Each TLC run gets its own directory under `build/tests/model/tlc/<test>/`, a copy of the models, its own `java.io.tmpdir`, two workers and a 60 second cap; its log is `tlc.log` there, and a trace run leaves its `WindowTraceData.tla` beside it.

One check by hand:

```sh
cmake -DWORK=/tmp/tlc-window -DMODELS=$PWD/models -DMODULE=MCWindow -DCFG=MCWindow.cfg -DEXPECT=pass \
      -P tests/model/tlc.cmake
```

## Adding the next layer's model

1. Extend `Window.tla`, or write a new module that extends it, with the new variables and actions. Keep refused operations as steps that change no model state and record the operation and its result in `last`.
2. State each operation's guarantee as an action property on `last'`, and the state rules as invariants. Add the properties to every config.
3. Add a witness per property: a module in `witnesses/` that passes one broken action to `NextWith` (widen `NextWith` with the new actions), and a config that checks only that property. Register it in `tests/model/CMakeLists.txt` with the property it must violate, then read its counterexample and confirm it fails for the reason intended.
4. Teach `trace_walk.cpp` the new operations and their arguments, and `WindowTrace.tla`'s `Step` the new record kinds. Keep a faulty subclass that the trace check rejects.
5. Keep each instance small enough that TLC finishes in seconds; bound growth with a state constraint, not with guards in the actions.
