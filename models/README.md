# models: the TLA+ specifications of nexus

Every state machine in nexus has a TLA+ module here. TLC checks each module on a small instance, with reversed witnesses, and the code is bound to the module: the code's own random walk writes traces, and TLC checks that each trace is a behaviour of the module.

This file covers the window's model. The server's durable state has its own directory: `store/` (its README), the layout of the journal under `src/store/`.

## What is modelled

`Window.tla` is the state the window owns, `nexus::Model` in `src/model.h`:

- `friends`: one tab per friend, fixed when the model is built. A friend is named by its place in that list, its id (`Tab::id` in the code), which never changes;
- `order`: the tabs' order, each position's tab id. A friend tab's id is its friend's; a terminal tab's id is one handed out by `AddTerminal`. Positions count from 0, as in the code;
- `nextId`: the next id `AddTerminal` hands out. Ids only grow, so no id is ever used twice;
- `active`: the active tab's position, 0 when there are no tabs;
- `transcript`: each friend's entries, kinds `Person`, `Friend`, `Notice`;
- `draft`: each friend's unsent text;
- `queue`: each friend's prompts held for later, oldest first. The queue is apart from the transcript: it is the state the server will hold, and no operation takes from it yet (delivery belongs to a later layer).

Transcripts, drafts and queues are indexed by friend, so they go with the tab when it moves; operations take positions, as the code's do, and act on `At(i)`, the friend at position i. The actions are the model's operations, with the arguments a caller passes, in range and out of it: `SelectTab(i)`, `SetDraft(i, t)`, `Send(i)` (the draft becomes a `Person` entry at the end of the tab's transcript), `Queue(i)` (the draft goes to the end of the tab's queue), `AppendEntry(i, e)`, `MoveTab(from, to)` (the tab at `from` ends at `to` and the tabs between shift by one; refused when the two positions lie in different zones), `AddTerminal` (a terminal tab at the end, made active), `CloseTab(i)` (removes the tab at i, only a terminal). A refused operation changes no model state.

Each tab kind has a zone (`Zone`): the sprint tab 0, friends 1, terminals 2. The order runs by zone, and a move stays inside its zone. The sprint tab is there when the constant `Sprint` is TRUE (the window's model always has it): it is made at the start in position 0 with id `SprintId` (N, the first after the friends'), alone in zone 0, so nothing moves before it, it never moves, and close refuses it. A terminal and the sprint tab hold no transcript, draft or queue: send, queue, set_draft and append refuse them.

`MoveTab` carries the code's index arithmetic, not only its result: the order is `Moved` (the code's `std::rotate`) and `active` is `MovedIndex` (the code's three-way adjustment of `active_`). The properties then state what that arithmetic must give in terms of friends: the same friend active, every friend kept once, the others in their relative order.

`last` is a history variable, not model state: the operation just performed, its arguments and the value it returned. The action properties are stated through it, and a code trace's return values are checked against it.

All-whitespace means every byte is an ASCII space, tab, newline, vertical tab, form feed or carriage return; the empty draft is all whitespace. The spec does not compute this: the constant `Blank` names the drafts that are all whitespace, and each instance lists them.

### Properties

| Property | Kind | What it says |
|---|---|---|
| `TypeOK` | invariant | every variable has its type; the friends are the ones the model was built with |
| `ActiveValid` | invariant | `active` is a tab, or 0 when there are none |
| `NoTabLost` | invariant | every friend has a tab in the order |
| `NoTabDuplicated` | invariant | no friend has two |
| `IdsHandedOut` | invariant | every tab's id is below `nextId` |
| `ZonesOrdered` | invariant | the tabs stand in zone order: no friend before the sprint tab, no terminal before a friend |
| `SprintFirst` | invariant | with the sprint tab, it is always there, always at position 0, always with its one id |
| `SprintTakesNoText` | action | set_draft, send, queue and append return false on the sprint tab |
| `SprintNeverMoves` | action | a move from or to the sprint tab's position changes neither the order nor `active` |
| `SprintNeverCloses` | action | close returns false on the sprint tab |
| `SelectExact` | action | select returns true exactly for a tab, and changes only `active`, to that tab |
| `SetDraftExact` | action | set_draft returns true exactly for a tab, and changes only that friend's draft, to the text given |
| `SendResult` | action | send and queue return true exactly for a tab whose friend's draft is not all whitespace |
| `SendAppendsOne` | action | a send that returns true appends one `Person` entry, with the draft's text, to that friend's transcript |
| `QueueAppendsOne` | action | a queue that returns true appends exactly the draft's text to that friend's queue |
| `QueueLeavesTranscripts` | action | a queue never changes any transcript |
| `OnlyQueueTouchesQueues` | action | send, select, set_draft, append and move never change a queue |
| `SendClearsOnlyItsDraft` | action | a send or queue that returns true clears that friend's draft and no other |
| `SendTouchesNoOtherTab` | action | it changes no other friend's transcript, draft or queue, nor the order, `active` or the friends |
| `AppendExact` | action | append returns true exactly for a tab, appends exactly that entry to that friend, and changes nothing else |
| `MovePlaces` | action | move returns true exactly when both positions are tabs of one zone; then the friend from `from` stands at `to` and the others keep their relative order |
| `MoveKeepsTabs` | action | a move changes no friend's transcript, draft or queue, nor the friends: they go with the tab |
| `OnlyMoveReorders` | action | no operation but move, add and close changes the order |
| `ActiveFriendKept` | action | every operation but select, add and close keeps the same tab active, wherever it now stands |
| `TerminalTakesNoText` | action | send, queue, set_draft and append return false on a terminal |
| `AddAppends` | action | add puts a new terminal at the end, makes it active and returns its id; the other tabs stay where they were |
| `AddFreshId` | action | the id add returns was never handed out before |
| `NextIdGrows` | action | `nextId` never goes down |
| `CloseRemovesExactly` | action | close returns true exactly for a terminal, and removes that tab and no other, the rest in their order |
| `CloseKeepsFriends` | action | a close changes no friend's transcript, draft or queue |
| `CloseActiveRule` | action | after a close the tab that was active stays active; if it was the one closed, the tab now at its position, else the last, else none |
| `RefusedChangesNothing` | action | an operation that returns false changes no model state (a refused move among them) |
| `TranscriptsOnlyGrow` | action | entries are only appended: existing entries never change, move or go |
| `QueuesOnlyGrow` | action | queued prompts are only appended: they never change, move or go |
| `FriendsFixed` | action | the friends never change |

### Instances

`MCWindow.tla` gives the constants values and bounds transcript and queue length (`MaxLen`, a state constraint). The drafts are `""`, `" "`, `" \t\n"` and `"hi"` (one witness adds `"yo"`, so that an order of queued prompts shows); the appended entries are a friend's reply, an empty notice and an all-whitespace `Person` entry (append takes any entry). The checked instances use fewer (`MCDrafts3`: `""`, `" "`, `"hi"`; on three tabs `MCAppendEntries3`: the reply only), because every order multiplies the states: the spec does not compute whitespace, so a second all-whitespace draft adds only states. Positions run to one past the last tab, for both of a move's. TLC tells states apart by the model variables alone (`VIEW ModelView`), which is sound because no action reads `last` and no property reads it unprimed.

| Config | Instance |
|---|---|
| `MCWindow.cfg` | the sprint tab, two friends and up to two terminals (`MaxTerms`, a bound on `nextId`), transcripts and queues up to 1 entry: 20,088 distinct states, about 8 s |
| `MCWindowNoSprint.cfg` | the same without the sprint tab |
| `MCWindow3Sprint.cfg` | the sprint tab, three friends in all six orders and up to one terminal, no entries: 2,106 distinct states, about 1 s |
| `MCWindow3.cfg` | three friends in all six orders, no sprint tab, no terminals, transcripts and queues up to 1 entry: 104,976 distinct states, about 28 s on two workers, 8 s on eight |
| `MCWindowLong.cfg` | two friends in both orders, no sprint tab, no terminals, transcripts and queues up to 2 entries: 142,884 distinct states, about 28 s on two workers, 8 s on eight |
| `MCWindowEmpty.cfg` | the sprint tab, no friends, up to two terminals: added, moved among themselves and closed |

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
| `WQueueToTranscript` | queue appends the draft to the transcript instead of the queue | `QueueLeavesTranscripts` |
| `WQueueWrongTab` | queue appends to the next tab's queue | `SendTouchesNoOtherTab` |
| `WSendAlsoQueues` | send also appends the draft to the queue | `OnlyQueueTouchesQueues` |
| `WQueuePrepends` | queue puts the draft at the front, reordering the queue | `QueuesOnlyGrow` |
| `WAppendDropsQueued` | append also drops the oldest queued prompt | `QueuesOnlyGrow` |
| `WSetDraftActiveToo` | set_draft writes the active tab's draft too | `SetDraftExact` |
| `WSelectClearsDraft` | select clears the draft of the tab it leaves | `SelectExact` |
| `WAppendWrongTab` | append puts the entry on the next tab | `AppendExact` |
| `WMoveDropsTab` | move takes the tab out and never puts it back | `NoTabLost` |
| `WMoveDuplicatesTab` | move puts the tab in at `to` without taking it out at `from` | `NoTabDuplicated` |
| `WMoveLeavesActive` | move leaves `active` at the old position, so another friend becomes active | `ActiveFriendKept` |
| `WMoveLeavesDraft` | move carries the friend but leaves each draft at its position | `MoveKeepsTabs` |
| `WMoveOutOfRange` | a move past the last tab returns false but moves the tab to the end | `RefusedChangesNothing` |
| `WMoveSwaps` | move swaps the two tabs instead of shifting those between (three tabs) | `MovePlaces` |
| `WSendByPosition` | send and queue take the position as the friend, as if no tab had moved | `SendTouchesNoOtherTab` |
| `WSetDraftOutsideDrafts` | set_draft stores a text it was not given | `TypeOK` |
| `WSendAppendsTwice` | send appends the `Person` entry twice | `SendAppendsOne` |
| `WQueueAppendsTwice` | queue appends the draft twice | `QueueAppendsOne` |
| `WSelectReorders` | select brings the selected tab to the front | `OnlyMoveReorders` |
| `WAppendRenamesFriend` | append also rewrites the friend's model | `FriendsFixed` |
| `WCloseRemovesFriend` | close removes a friend's tab too | `NoTabLost` |
| `WCloseWrongTab` | close removes the last tab instead of the one named | `CloseRemovesExactly` |
| `WCloseActivePastEnd` | closing the active last tab leaves `active` past the end | `ActiveValid` |
| `WCloseActiveWrong` | closing the active tab makes the tab before it active | `CloseActiveRule` |
| `WCloseDropsDraft` | close also clears every friend's draft | `CloseKeepsFriends` |
| `WAddReusesId` | add hands out the lowest id not in use, so a closed id comes back | `AddFreshId` |
| `WAddAmongTerminals` | add puts the new terminal in front of the last terminal | `AddAppends` |
| `WAddBeforeFriend` | add puts the new terminal first | `ZonesOrdered` |
| `WSendToTerminal` | send accepts a terminal position | `TerminalTakesNoText` |
| `WMoveTerminalBeforeFriend` | move lets a terminal go in front of a friend | `ZonesOrdered` |
| `WMoveFriendBehindTerminal` | move lets a friend go behind a terminal | `ZonesOrdered` |
| `WMoveCrossZoneMoves` | a move across zones returns false but moves the tab | `RefusedChangesNothing` |
| `WMoveBeforeSprint` | move lets a friend go in front of the sprint tab | `SprintFirst` |
| `WMoveSprint` | move takes the sprint tab among the friends | `SprintNeverMoves` |
| `WCloseSprint` | close accepts the sprint tab | `SprintNeverCloses` |
| `WSendToSprint` | send accepts the sprint tab's position | `SprintTakesNoText` |

Every property in the table above has a witness.

## The binding: code traces checked against the spec

`tests/model/trace_walk.cpp` drives the real `nexus::Model` with a seeded random walk (`trace_walk <seed> <steps> <friends> [--no-sprint] [--faulty | --faulty-queue | --faulty-move | --faulty-queue-cap | --faulty-send-trim | --faulty-send-dedup | --faulty-close | --faulty-sprint-move | --faulty-sprint-close] <out.tla>`). The model starts with the sprint tab, as the window's does, unless `--no-sprint`. The random numbers come from the seed alone. Each step calls one operation (adding and closing terminals among them, at most three open), with positions in range and two past the last tab (a move draws both, so moves across zones are tried and refused), drafts that are empty, all whitespace (including tab, newline, vertical tab, carriage return and form feed), text with quotes and backslashes, and text with whitespace before, after and around it, and entries of every kind. Two kinds of step repeat the last accepted send or queue with the same text to the same tab (a `set_draft`, then the `send` or `queue`), so that equal sends follow each other and queues grow long; the walk prints the longest queue (dozens of prompts at 1000 steps). The walk writes `WindowTraceData.tla`: the arguments' domains and `Trace`, one record per step, holding the operation, its arguments, the return value and the model's whole state after it: the order (each position's `Tab::id`) and, by position, each tab's kind, friend, draft, transcript and queue.

`WindowTrace.tla` instantiates `Window` with those domains. Each of its steps takes the `Window` action the record names, with the record's arguments, and the invariant `TraceMatches` requires the spec's state and return value to equal the code's: the order equal, and the tab at each position holding the state of the friend the spec has there. A step whose arguments lie outside the spec's domains has no successor and TLC reports a deadlock. The property `WindowSpec` checks the whole behaviour against `Window`'s `Init` and `Next`, and every `Window` property is checked on the trace too.

The binding is shown to fail, nine times: `--faulty-sprint-move` walks a subclass whose `move_tab` takes the sprint tab among the friends, `--faulty-sprint-close` one whose `close_tab` closes it; `--faulty` walks a test-only subclass whose `send` accepts a draft that is all whitespace but not empty, `--faulty-queue` one whose `queue` also puts the draft in the transcript, `--faulty-move` one whose `move_tab` leaves `active` at the old position, `--faulty-queue-cap` one whose queue drops its oldest prompt past four, `--faulty-send-trim` one whose `send` strips trailing spaces, `--faulty-send-dedup` one whose `send` of the text just sent appends nothing, and `--faulty-close` one whose `close_tab` also removes a friend's tab. TLC must report `TraceMatches` violated at the first step where each fault shows.

## Link.tla: what nexus adds on top of mosh

`Link.tla` models the link between the windows and `nexus-server` (`src/sync`, `src/link`, `src/server`): what nexus puts on top of mosh's State Synchronization Protocol, not mosh. mosh is abstracted to the two things nexus relies on. Up, what reaches the server of a window's list is some range of it, which may repeat a range, come after a later one, or start past what the server has: `Deliver(c, first, last)` takes any range. Down, a window's receiver eventually holds the server's newest state, may skip states between, and never goes back to an older one (mosh numbers its states and keeps the newest): `Receive(c, j)` is window c receiving the server's j-th state, the same as before or a later one.

- Up: `acts[c]`, window c's actions, numbered from 1 (`sync::Actions`); `applied[c]`, the server's `sync::Applier` for it; `done[c]`, a history variable, every number the server applied, in order. The Applier applies the numbers past `applied[c]` in a range, refuses one that starts past `applied[c] + 1`, and ignores one wholly applied.
- The server: `tr[t]` and `q[t]`, each tab's transcript and queue (`sync::Hub` over its store). A send puts the prompt and a placeholder reply in the transcript; a queue puts the prompt in the queue. Each change makes a new state in `snaps`: each transcript's newest `W` entries from index `first` (`sync::tailOf`, by count; the byte budget is not modelled) and the whole queue.
- Down: `last[c]`, the state window c's receiver holds; `view[c][t]`, `vq[c][t]`, the window's transcript and queue; `seen[c][t]`, its mirror (`sync::Mirror`): the transcript index it shows through and the number of queued prompts it shows. Receiving a state, the mirror appends a notice of how many entries left the tail before it saw them, then the entries it has not shown, then the queued prompts it has not shown.

Entries are records: a prompt `[k |-> "P", text |-> <<c, n>>]` (action n of window c), its reply `[k |-> "F", text |-> <<>>]`, a notice of m entries not shown `[k |-> "N", text |-> <<m>>]`.

Not modelled: mosh (sockets, crypto, fragments, timers, acknowledgements, its own correctness); the hub's limits (a refused action becomes a notice in the transcript, which the mirror shows like any entry); entries that grow while open (the server makes none yet).

| Property | Kind | What it says |
|---|---|---|
| `TypeOK` | invariant | counts and indices in range |
| `ExactlyOnce` | invariant | the server applied window c's actions 1, 2, ..., `applied[c]`, each once, in order, none it was not given |
| `ServerHoldsEach` | invariant | every applied action is in the server's tabs exactly once, where its kind puts it; none not applied is |
| `ViewMatches` | invariant | a window's transcript, with each notice of m read as m holes, is the server's transcript through what it has seen: each entry at its index |
| `QueueMatches` | invariant | a window's queue is the server's, through what it has seen |
| `NoticesSound` | invariant | a notice stands for at least one entry, and two never follow each other |
| `OnlyGrow` | action | the server's transcripts and queues, each window's, and the applied history only grow; a receiver never goes back |
| `Converges` | liveness | under mosh's promise (fairness: a window's whole list reaches the server, the newest state reaches every window), every action is applied and every window shows all of every transcript and queue |

Instances: `MCLink.cfg`, two windows sharing a tab, two actions each, a tail of one entry (16,641 states, 2 s on four workers); `MCLinkOne.cfg`, one window over two tabs, three actions, a tail of two (1,491 states); `MCLinkWide.cfg`, nightly, two windows over two tabs (183,177 states, 29 s on four workers).

Reversed witnesses (`witnesses/WLink*.tla`), each rejected by TLC:

| Witness | The break | Rejected by |
|---|---|---|
| `WLinkApplyAgain` | the Applier applies a range from its first number, not from `applied + 1` | `ExactlyOnce` |
| `WLinkAcceptGap` | the Applier takes a range past a gap | `ExactlyOnce` |
| `WLinkMirrorAgain` | the mirror appends the whole tail each time | `ViewMatches` |
| `WLinkNoNotice` | the mirror skips the notice for entries it missed | `ViewMatches` |
| `WLinkQueueAgain` | the mirror queues the whole queue each time | `QueueMatches` |
| `WLinkTailMisaligned` | the server's tail names its first entry one early | `ViewMatches` |
| `WLinkDeliverShort` | the Applier stops one short of each range | `Converges` |

The binding: `tests/sync/link_walk.cpp` (`link_walk <seed> <steps> <windows> [--faulty-applier | --faulty-mirror | --faulty-tail] <out.tla>`) runs the real `sync::Actions`, `Applier`, `Hub` over a `MemoryStore`, `Tabs`, `Mirror` and `nexus::Model` through `tests/sync/fake_mosh.h`, a copy of mosh 1.4.0's sender and receiver bookkeeping, with datagrams lost, repeated and reordered from the seed. Now and then it also hands the Applier a range mosh would not: one already applied, or one past a gap. It records each window action, each range the Applier is given and each state a window mirrors, with the code's whole state after it, and ends by letting everything through. `LinkTrace.tla` takes the `Link` action each record names, and `TraceMatches` requires the spec's state to equal the code's; every `Link` safety property is checked on the trace, and its last state must be `Settled`. The binding is shown to fail three times: `--faulty-applier` applies a whole range even where it was applied before, `--faulty-mirror` mirrors with a new `Mirror` each time, and `--faulty-tail` makes the server's tail one entry longer than the spec's; TLC must report `TraceMatches` violated.

Per commit (`-L '^model$'`): the two small instances, the seven witnesses, six traces (seeds 1 to 4 on two windows and seed 5 on one, 400 steps each; seed 6 on three windows, 600 steps) and the three faulty traces: under 26 s of TLC in all, 7 s on eight cores.

## Running the checks

TLC needs Java 11 or newer and `tla2tools.jar`. The tests find the jar through `TLA2TOOLS_JAR`, and java through `JAVA` or `PATH`. Without `TLA2TOOLS_JAR` every `model` test fails and says what to set.

```sh
cmake -S . -B build
cmake --build build --target model_checks -j4
ctest --test-dir build -L unit
TLA2TOOLS_JAR=/path/to/tla2tools.jar ctest --test-dir build -L model -j "$(sysctl -n hw.ncpu)"
```

`-L model` matches two labels: `model` runs four instances (`MCWindow`, `MCWindowEmpty`, `MCWindowNoSprint`, `MCWindow3Sprint`), the forty-two witnesses, nine code traces (with the sprint tab first: seeds 1 to 5 on three friends, seed 6 on two friends and seed 8 on one, each 1000 steps, and seed 7 with no friends; without it: seed 9 on three friends), and the nine faulty traces; `model_nightly` runs the two largest instances, `MCWindow3` and `MCWindowLong`. CI's per-commit job runs `-L '^model$'` and the nightly workflow `-L '^model_nightly$'`: every property is still checked on every commit, on the smaller instances. Each TLC run gets its own directory under `build/tests/model/tlc/<test>/`, a copy of the models, its own `java.io.tmpdir`, two workers (eight on the four instances with tens of thousands of states, which ctest counts as eight of its `-j` slots) and a 60 second cap (300 on the nightly instances); its log is `tlc.log` there, and a trace run leaves its `WindowTraceData.tla` beside it.

One check by hand:

```sh
cmake -DWORK=/tmp/tlc-window -DMODELS=$PWD/models -DMODULE=MCWindow -DCFG=MCWindow.cfg -DEXPECT=pass \
      -P tests/model/tlc.cmake
```

## Adding the next layer's model

1. Extend `Window.tla`, or write a new module that extends it, with the new variables and actions. Keep refused operations as steps that change no model state and record the operation and its result in `last`.
2. State each operation's guarantee as an action property on `last'`, and the state rules as invariants. Add the properties to every config.
3. Add a witness per property: a module in `witnesses/` that passes one broken action to `NextWithAll` (widen it with the new actions), and a config that checks only that property. Register it in `tests/model/CMakeLists.txt` with the property it must violate, then read its counterexample and confirm it fails for the reason intended.
4. Teach `trace_walk.cpp` the new operations and their arguments, and `WindowTrace.tla`'s `Step` the new record kinds. Keep a faulty subclass that the trace check rejects.
5. Keep each instance small enough that TLC finishes in seconds; bound growth with a state constraint, not with guards in the actions.
