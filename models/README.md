# models: the TLA+ specifications of nexus

Every state machine in nexus has a TLA+ module here (`Overlay.tla`, when the overlay shows and hides, among them). TLC checks each module on a small instance, with reversed witnesses, and the code is bound to the module: the code's own random walk writes traces, and TLC checks that each trace is a behaviour of the module.

This file covers the window's model. The server's durable state has its own directory: `store/` (its README), the layout of the journal under `src/store/`.

## What is modelled

`Window.tla` is the state the window owns, `nexus::Model` in `src/model.h`:

- `friends`: one tab per friend, fixed when the model is built. A friend is named by its place in that list, its id (`Tab::id` in the code), which never changes;
- `order`: the tabs' order, each position's tab id. A friend tab's id is its friend's; a terminal tab's id is one handed out by `AddTerminal`. Positions count from 0, as in the code;
- `nextId`: the next id `AddTerminal` hands out. Ids only grow, so no id is ever used twice;
- `active`: the active tab's position, 0 when there are no tabs;
- `history`: the selection history, `Model::selection_history()`: the ids of the tabs selected, most recent last, each once, open tabs only. The tab active at the start is its first entry; select and add put their tab's id at its end, and close takes the closed tab's id out. Closing the active tab makes the last id left in it active, the tab selected before it, whatever its kind; with none left, the rule before it (the tab now at its position, or the last). It is kept for the run only;
- `transcript`: each friend's entries, kinds `Person`, `Friend`, `Notice`;
- `draft`: each friend's unsent text;
- `queue`: each friend's prompts held for later, oldest first. The queue is apart from the transcript: it mirrors the queue the server holds, and its oldest prompt leaves it only by `TakeQueued`, when the server delivered it (the window's mirror's call; `Link.tla` models the delivery).

Transcripts, drafts and queues are indexed by friend, so they go with the tab when it moves; operations take positions, as the code's do, and act on `At(i)`, the friend at position i. The actions are the model's operations, with the arguments a caller passes, in range and out of it: `SelectTab(i)`, `SetDraft(i, t)`, `Send(i)` (the draft becomes a `Person` entry at the end of the tab's transcript), `Queue(i)` (the draft goes to the end of the tab's queue), `TakeQueued(i)` (the oldest prompt of the tab's queue leaves it), `AppendEntry(i, e)`, `MoveTab(from, to)` (the tab at `from` ends at `to` and the tabs between shift by one; refused when the two positions lie in different zones), `AddTerminal` (a terminal tab at the end, made active), `CloseTab(i)` (removes the tab at i, only a terminal). A refused operation changes no model state.

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
| `HistoryOpen` | invariant | the history holds only ids of tabs that stand: never a closed tab's, never one not handed out |
| `HistoryOnce` | invariant | the history holds each id once |
| `HistoryEndsActive` | invariant | while there are tabs, the history's last id is the active tab's |
| `SprintTakesNoText` | action | set_draft, send, queue and append return false on the sprint tab |
| `SprintNeverMoves` | action | a move from or to the sprint tab's position changes neither the order nor `active` |
| `SprintNeverCloses` | action | close returns false on the sprint tab |
| `SelectExact` | action | select returns true exactly for a tab, and changes only `active`, to that tab |
| `SetDraftExact` | action | set_draft returns true exactly for a tab, and changes only that friend's draft, to the text given |
| `SendResult` | action | send and queue return true exactly for a tab whose friend's draft is not all whitespace |
| `SendAppendsOne` | action | a send that returns true appends one `Person` entry, with the draft's text, to that friend's transcript |
| `QueueAppendsOne` | action | a queue that returns true appends exactly the draft's text to that friend's queue |
| `QueueLeavesTranscripts` | action | a queue never changes any transcript |
| `OnlyQueueTouchesQueues` | action | send, select, set_draft, append and move never change a queue: only queue and take do |
| `TakeExact` | action | take returns true exactly for a friend's tab with a queued prompt; then that queue's oldest prompt leaves it, and nothing else changes |
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
| `CloseActiveRule` | action | after a close the tab that was active stays active; if it was the one closed, the last id left in the history (the tab selected before it); with none left, the tab now at its position, else the last, else none |
| `SelectMovesToEnd` | action | a select that returns true, and every add, puts its tab's id at the end of the history and takes it out of where it stood |
| `CloseForgets` | action | a close that returns true takes the closed tab's id out of the history, and nothing else |
| `RefusedChangesNothing` | action | an operation that returns false changes no model state (a refused move among them) |
| `TranscriptsOnlyGrow` | action | entries are only appended: existing entries never change, move or go |
| `QueuesOnlyGrow` | action | but for a take, queued prompts are only appended: they never change, move or go |
| `FriendsFixed` | action | the friends never change |

### Instances

`MCWindow.tla` gives the constants values and bounds transcript and queue length (`MaxLen`, a state constraint). The drafts are `""`, `" "`, `" \t\n"` and `"hi"` (one witness adds `"yo"`, so that an order of queued prompts shows); the appended entries are a friend's reply, an empty notice and an all-whitespace `Person` entry (append takes any entry). The checked instances use fewer (`MCDrafts3`: `""`, `" "`, `"hi"`; on three tabs `MCAppendEntries3`: the reply only), because every order multiplies the states: the spec does not compute whitespace, so a second all-whitespace draft adds only states. Positions run to one past the last tab, for both of a move's. TLC tells states apart by the model variables alone (`VIEW ModelView`), which is sound because no action reads `last` and no property reads it unprimed.

Every order of the selection history is a state of its own, so the history multiplies the instances with many tabs. The times are the studio's in a login session; CI's runners take about four times as long.

| Config | Instance |
|---|---|
| `MCWindowSprint.cfg` | the sprint tab, two friends and up to two terminals (`MaxTerms`, a bound on `nextId`), no entries: 8,910 distinct states, about 7 s on two workers (it runs on eight) |
| `MCWindow.cfg` | the same with transcripts and queues up to 1 entry: 320,760 distinct states, about 37 s on eight workers (nightly) |
| `MCWindowNoSprint.cfg` | two friends and up to two terminals without the sprint tab, entries up to 1: 60,264 distinct states, about 5 s on eight workers |
| `MCWindow3Sprint.cfg` | the sprint tab, three friends in all six orders and up to one terminal, no entries, two drafts (`MCDrafts2`: empty and text): 14,880 distinct states, about 12 s on two workers (it runs on eight) |
| `MCWindow3.cfg` | three friends in all six orders, no sprint tab, no terminals, transcripts and queues up to 1 entry: 384,912 distinct states, about 27 s on eight workers (nightly) |
| `MCWindowLong.cfg` | two friends in both orders, no sprint tab, no terminals, transcripts and queues up to 2 entries: 214,326 distinct states, about 12 s on eight workers (nightly) |
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
| `WCloseActiveWrong` | closing the active tab makes the tab before it in the strip active | `CloseActiveRule` |
| `WCloseOldRule` | closing the active tab ignores the history: the tab now at its position, or the last, so closing the last terminal goes back to the last friend (with the sprint tab) | `CloseActiveRule` |
| `WCloseKeepsHistory` | close leaves the closed tab's id in the history | `HistoryOpen` |
| `WCloseForgetsAll` | close empties the whole history | `CloseForgets` |
| `WSelectKeepsPlace` | select records its tab only the first time, so a tab selected again keeps its old place | `SelectMovesToEnd` |
| `WSelectAppendsAgain` | select appends its tab's id without taking it out of where it stood | `HistoryOnce` |
| `WSelectSkipsHistory` | select never records its tab | `HistoryEndsActive` |
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
| `WTakeNewest` | take removes the newest queued prompt, not the oldest (passes `NextTake`) | `TakeExact` |

Every property in the table above has a witness.

## The binding: code traces checked against the spec

`tests/model/trace_walk.cpp` drives the real `nexus::Model` with a seeded random walk (`trace_walk <seed> <steps> <friends> [--no-sprint] [--faulty | --faulty-queue | --faulty-move | --faulty-queue-cap | --faulty-send-trim | --faulty-send-dedup | --faulty-close | --faulty-sprint-move | --faulty-sprint-close | --faulty-close-fallback] <out.tla>`). The model starts with the sprint tab, as the window's does, unless `--no-sprint`. The random numbers come from the seed alone. Each step calls one operation (adding and closing terminals among them, at most three open), with positions in range and two past the last tab (a move draws both, so moves across zones are tried and refused), drafts that are empty, all whitespace (including tab, newline, vertical tab, carriage return and form feed), text with quotes and backslashes, and text with whitespace before, after and around it, and entries of every kind. Two kinds of step repeat the last accepted send or queue with the same text to the same tab (a `set_draft`, then the `send` or `queue`), so that equal sends follow each other and queues grow long; the walk prints the longest queue (dozens of prompts at 1000 steps). Every 25th step is followed by a `take_queued` at the same position, a step that draws no random number, so the other steps are as they were. The walk writes `WindowTraceData.tla`: the arguments' domains and `Trace`, one record per step, holding the operation, its arguments, the return value and the model's whole state after it: the order (each position's `Tab::id`), the selection history and, by position, each tab's kind, friend, draft, transcript and queue.

`WindowTrace.tla` instantiates `Window` with those domains. Each of its steps takes the `Window` action the record names, with the record's arguments, and the invariant `TraceMatches` requires the spec's state and return value to equal the code's: the order, the active position and the history equal, and the tab at each position holding the state of the friend the spec has there. A step whose arguments lie outside the spec's domains has no successor and TLC reports a deadlock. The property `WindowSpec` checks the whole behaviour against `Window`'s `Init` and `Next`, and every `Window` property is checked on the trace too.

The binding is shown to fail, ten times: `--faulty-close-fallback` walks a subclass whose `close_tab` of the active tab ignores the history and keeps the rule before it (rejected at the first close of an active tab where the two rules differ); `--faulty-sprint-move` walks a subclass whose `move_tab` takes the sprint tab among the friends, `--faulty-sprint-close` one whose `close_tab` closes it; `--faulty` walks a test-only subclass whose `send` accepts a draft that is all whitespace but not empty, `--faulty-queue` one whose `queue` also puts the draft in the transcript, `--faulty-move` one whose `move_tab` leaves `active` at the old position, `--faulty-queue-cap` one whose queue drops its oldest prompt past four, `--faulty-send-trim` one whose `send` strips trailing spaces, `--faulty-send-dedup` one whose `send` of the text just sent appends nothing, and `--faulty-close` one whose `close_tab` also removes a friend's tab. TLC must report `TraceMatches` violated at the first step where each fault shows.

## TabLabel.tla: a terminal tab's label across ssh sessions

`TabLabel.tla` is `nexus::TabLabel` in `src/remote.h`: one terminal tab's label. Once a frame the window observes the terminal: the remote label of its foreground process (the fleet machine its ssh reaches, its host if not a fleet machine, or none when the foreground is not ssh), the program's title, and `late`, whether the frame comes after the tail of the last session to end (the code compares the frame's time with that end plus `kSessionTailSeconds`). Its state is the code's members: `remote`, `before` (the title at the last observation outside sessions and masks), `after` (the title a session left), `masking` and `tail`; `last` records the observation. `Label(t)` is the label drawn when the title is `t`.

| Property | Kind | What it says |
|---|---|---|
| `TypeOK` | invariant | every variable has its type |
| `TailMasks` | invariant | a tail is part of a mask |
| `SessionNamed` | invariant | while a session is seen, the label is its remote label, whatever the title |
| `TitleShows` | invariant | outside sessions and masks, the label is the title |
| `SessionNamedAtOnce` | action | an observation of a session names it at once |
| `EndRestores` | action | the observation that sees a session end gives back the label from before it, whatever title it left |
| `TailRestores` | action | in the session's tail the label stays the one from before, whatever title arrives |
| `MaskEnds` | action | past the tail, the first title other than the one the session left is shown: the label never sticks |
| `BeforeIsOwn` | action | the label to return to only ever changes to a title observed outside sessions and masks |
| `BeforeIsLatest` | action | and every such observation takes it |

`TabLabel.cfg` checks them on two remote labels and three titles (295 states). Its witnesses: `WLabelNoMask` (a session's end shows the title it left; `EndRestores`), `WLabelMaskSticks` (the mask never lifts; `MaskEnds`), `WLabelBeforeAtStart` (the label to return to is the title at a session's first observation, which ssh may already have set; `BeforeIsOwn`), `WLabelNoTail` (no tail, so a last title read after the process is seen gone shows; `TailRestores`).

`tests/model/label_walk.cpp` (`label_walk <seed> <steps> <remotes> [--faulty-before] <out.tla>`) drives the real `TabLabel` with a seeded walk on a made-up clock (a frame or a pause a step): sessions start, run, switch and end, titles change, and frames fall inside tails and after them. It writes `TabLabelTraceData.tla`: one record per observation with its arguments, the code's state after it and the label the code draws for every title. `TabLabelTrace.tla` takes `Observe` with each record's arguments and `TraceMatches` requires the spec's state and every label to equal the code's; every `TabLabel` property is checked on the trace. Four traces run (seeds 1 to 3 on two remotes, seed 4 on three), and `--faulty-before`, which feeds the code an unrecorded observation before each session's first (the code then takes the label to return to from a title ssh may have set), must be rejected with `TraceMatches` violated.

## Link.tla: what nexus adds on top of mosh

`Link.tla` models the link between the windows and `nexus-server` (`src/sync`, `src/link`, `src/server`): what nexus puts on top of mosh's State Synchronization Protocol, not mosh. mosh is abstracted to the two things nexus relies on. Up, what reaches the server of a window's list is some range of it, which may repeat a range, come after a later one, or start past what the server has: `Deliver(c, first, last)` takes any range. Down, a window's receiver eventually holds the server's newest state, may skip states between, and never goes back to an older one (mosh numbers its states and keeps the newest): `Receive(c, j)` is window c receiving the server's j-th state, the same as before or a later one.

- Up: `acts[c]`, window c's actions, numbered from 1 (`sync::Actions`): a send, a queue or a stop; `applied[c]`, the server's `sync::Applier` for it; `done[c]`, a history variable, every number the server applied, in order. The Applier applies the numbers past `applied[c]` in a range, refuses one that starts past `applied[c] + 1`, and ignores one wholly applied.
- The server: `tr[t]` and `q[t]`, each tab's transcript and queue (`sync::Hub` over its store, durable); `op[t]`, the tab's open entry, the friend's text still growing (`Hub::setOpen`, in memory only); `stops[t]`, a history variable, every stop applied to the tab, and whether it reached the tab's responder (the drivers) or was refused because the server holds no such tab. A send puts the prompt and a placeholder reply in the transcript; a queue puts the prompt in the queue; a stop changes no tab. The server's own steps: `Grow(t)` (an open entry is made, numbered, or grows), `Close(t)` (the open entry is closed into the transcript, all its text, and is gone), `Take(t)` (with no entry open, the queue's oldest prompt is delivered: it leaves the queue and is the person's entry in the transcript), `Restart` (the server starts again from its store: every open entry is lost and its turn cut with a notice). `made` counts the open entries made, the grows and the restarts. Each change makes a new state in `snaps`: each transcript's newest `W` entries from index `first` (`sync::tailOf`, by count; the byte budget is not modelled), then the open entry, which the tail does not count, so the newest closed entry always comes with it; and the whole queue.
- Down: `last[c]`, the state window c's receiver holds; `view[c][t]`, `vq[c][t]`, the window's transcript and queue; `seen[c][t]`, its mirror (`sync::Mirror`): the transcript index it shows through; `live[c][t]`, the open entry the mirror holds apart from the transcript (`Mirror::open`). Receiving a state, the mirror appends a notice of how many entries left the tail before it saw them, then the closed entries it has not shown, stopping at the open entry, which it holds; and makes its queue the state's (the prompts the server took leave its head, `Model::take_queued`, and those queued since go at its end).

Entries are records: a prompt `[k |-> "P", text |-> <<c, n>>]` (action n of window c, sent, or queued and delivered), its reply `[k |-> "F", text |-> <<>>]`, the friend's grown text open `[k |-> "O", text |-> <<g, len>>]` and closed `[k |-> "G", text |-> <<g, len>>]` (the g-th open entry, of len pieces), the notice of a turn a restart cut, which lost open entry g, `[k |-> "X", text |-> <<g>>]`, a notice of m entries not shown `[k |-> "N", text |-> <<m>>]`.

Not modelled: mosh (sockets, crypto, fragments, timers, acknowledgements, its own correctness); the hub's limits (a refused action becomes a notice in the transcript, which the mirror shows like any entry); the turns themselves and what ends them (`Supervisor.tla`): here the server's own steps happen whenever their guards allow. So a restart cuts only a turn with an open entry; the code also cuts an open turn with no text yet, or whose text is all closed, and that notice is an entry the server appends like any other, which the properties on the transcript already cover. The binding stands a responder in for the drivers: the supervisor's take and stop are bound by `Supervisor.tla`'s trace check, while how `Drivers` turns its output into the hub's changes (the take first, then the effects, then the driver's state) is tested end to end by `friends_functional_queue`, `friends_functional_queue_unconfigured` and `friends_functional_stop`, not trace-checked.

How much is durable, and when (the decision behind `op`): the open entry is in memory only, and becomes a durable closed entry at its block's end (text-end, a tool line, the turn's done or cut, or the entry limit: one journal record per block, never one per delta). A restart loses at most the open block's text, and the restart's cut notice says the reply was cut; a closed entry is in the store once, and the mirror appends only closed entries, each index once.

| Property | Kind | What it says |
|---|---|---|
| `TypeOK` | invariant | counts and indices in range |
| `ExactlyOnce` | invariant | the server applied window c's actions 1, 2, ..., `applied[c]`, each once, in order, none it was not given |
| `ServerHoldsEach` | invariant | every applied action is in the server's tabs exactly once, where its kind puts it (a queued prompt in the queue or, delivered, in the transcript, never both; a stop among its tab's stops, reaching the responder exactly when the server held the tab); none not applied is |
| `EachClosedOnce` | invariant | each open entry ends in the transcript at most once, closed or cut, and never while it is still open |
| `ViewMatches` | invariant | a window's transcript, with each notice of m read as m holes, is the server's transcript through what it has seen: each entry at its index, never an open one |
| `QueueMatches` | invariant | a window's queue is the queue of the state it holds |
| `NoticesSound` | invariant | a notice stands for at least one entry, and two never follow each other |
| `LiveSound` | invariant | the open entry a window holds is text the server had: the open entry still growing, the entry it closed into, or the notice of the restart that lost it |
| `OnlyGrow` | action | the server's transcripts, each window's, the stops and the applied history only grow; a queue only grows at its end or loses its oldest prompt to the transcript, as the person's entry; a receiver never goes back |
| `OpenGrows` | action | the server's open entry only grows, and goes only into the transcript, closed with all its text or cut |
| `Converges` | liveness | under mosh's promise (fairness: a window's whole list reaches the server, the newest state reaches every window), every action is applied and every window shows all of every transcript, the open entry and the queue |

Instances (states and times on four workers in a login session on the studio): `MCLink.cfg`, two windows sharing a tab, one action each of any kind, an open entry grown once and a restart (29,848 states, 5 s); `MCLinkOne.cfg`, one window over two tabs, three actions, a tail of two (12,373 states, 2 s); `MCLinkGrow.cfg`, one window, one tab, two actions of any kind, an open entry grown twice and a restart (17,420 states, 2 s); `MCLinkTabs.cfg`, one window over two tabs, two sends, open entries grown three times in all and a restart (54,244 states, 8 s); nightly, `MCLinkStops.cfg`, two windows sharing a tab, two actions each of any kind, no open entries (643,703 states, 1 min 53 s), and `MCLinkWide.cfg`, two windows over two tabs, one action each of any kind, an open entry grown once and a restart (132,194 states, 37 s). The constant `Kinds` names the kinds of action an instance's windows make.

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
| `WLinkMirrorOpen` | the mirror appends the open entry to the transcript as if it were closed | `ViewMatches` |
| `WLinkCloseKeepsOpen` | the server closes the open entry into the transcript but keeps showing it open | `EachClosedOnce` |
| `WLinkTakeNewest` | at a turn's end the server delivers the newest queued prompt, not the oldest | `OnlyGrow` |
| `WLinkRestartNoCut` | a restart drops the open entry without the notice that cuts its turn | `LiveSound` |
| `WLinkStopTwice` | the hub hands a stop to the responder twice | `ServerHoldsEach` |

The binding: `tests/sync/link_walk.cpp` (`link_walk <seed> <steps> <windows> [--faulty-applier | --faulty-mirror | --faulty-tail | --faulty-close] <out.tla>`) runs the real `sync::Actions`, `Applier`, `Hub` over a `MemoryStore` with a responder standing for the drivers, `Tabs`, `Mirror` and `nexus::Model` through `tests/sync/fake_mosh.h`, a copy of mosh 1.4.0's sender and receiver bookkeeping, with datagrams lost, repeated and reordered from the seed. Now and then it also hands the Applier a range mosh would not: one already applied, or one past a gap. The server's own steps are the code's calls the drivers make: `Hub::setOpen` for a grow, a closed entry committed and then the open one dropped for a close (as `Drivers::run` does), `takeQueued` and the person's entry in one step for a take, and for a restart a new `Hub` over the same store with each lost turn's cut notice committed. It records each window action, each range the Applier is given, each state a window mirrors and each of the server's own steps, with the code's whole state after it, and ends by letting everything through. `LinkTrace.tla` takes the `Link` action each record names, and `TraceMatches` requires the spec's state to equal the code's; every `Link` safety property is checked on the trace, and its last state must be `Settled`. The binding is shown to fail four times: `--faulty-applier` applies a whole range even where it was applied before, `--faulty-mirror` mirrors with a new `Mirror` each time, `--faulty-tail` makes the server's tail one entry longer than the spec's, and `--faulty-close` closes an entry without dropping the open one; TLC must report `TraceMatches` violated.

Per commit (`-L '^model$'`): the four small instances, the twelve witnesses, six traces (seeds 1 to 4 on two windows and seed 5 on one, 400 steps each; seed 6 on three windows, 600 steps) and the four faulty traces: 26 tests, 34 s on four cores.

## Supervisor.tla: a friend tab's driver

`Supervisor.tla` models `src/server/supervisor.h`, the state machine `nexus-server` keeps per friend tab: it starts the tab's driver when a prompt waits, writes each prompt as the next turn, gathers the friend's text, stops a turn, and ends the driver (input closed, then TERM, then KILL, each after its grace period). The driver is the environment: any message of any kind, for the open turn or another, a line the server refuses, a pipe closing, an exit at any moment. A driver that reports `no-credential` (protocol rule 11) is ended once no turn is open, and no driver is started again until the person's next prompt. Deadlines are steps that may happen (no hello, no done after a stop, each grace while ending); a working turn has none. `Restart` is the server dying and starting again from what the store holds: the next turn number, the open turn, the last session id, the waiting prompts and the queue. `Shutdown` is the server stopping. The queue is the person's prompts held for later (Command-Enter), the hub's queue of the tab; the waiting prompts are those a send left for the driver. `QueuePrompt` puts a prompt at the queue's end, and `Take`, after every event, moves the queue's oldest prompt to the waiting prompts once no turn is open and nothing waits, shown as the person's entry: one per turn, oldest first, each once. So a prompt queued with the friend idle goes at once, a sent prompt waiting goes before the queue, and a stop ends the turn and lets the next queued prompt go. Not modelled: the server also takes nothing while the friend has no driver configured (its drivers file lost it): the model's driver is always configured, and that guard only makes `Take` enabled less often, so every safety property still holds; `supervisor_unit_queue` and `friends_functional_queue_unconfigured` and `friends_functional_restart_unconfigured` test it. `Stop` ends what runs when it arrives: it writes one stop for the open turn, or, while the driver starts, cancels the turn about to begin (its prompt leaves the waiting prompts, never written, with a notice; the driver keeps starting for the next). One that finds the turn already stopping, or nothing running, changes nothing: no stop is kept for a later turn. `stops`, a history variable, records each stop written, and `asked` marks the step of the person's stop.

Invariants: `TypeOK`; `OneDriver` (at most one driver process, alive exactly when the phase says so); `OneTurn` (at most one turn open, only while a driver works on it); `TurnsIncrease` (a turn number is never reused, across driver and server restarts); `AtMostOnce` (each prompt is written to a driver at most once, ever); `WaitingSound` (a prompt waits only while the driver cannot take it, within the bound); `LimitHolds` (the failed-start count stays within its limit); `QueueSound` (the queue within its bound, each prompt in it once, and in it exactly while queued); `StopOnce` (at most one stop written per turn). Action properties: `StopForOpen` (a stop is written only for the open turn), `StopEndsWhatRuns` (a stop ends what runs when it arrives: the open turn, or while the driver starts the turn about to begin, its prompt cancelled and never written; finding the turn already stopping, or nothing running, it changes nothing), `NoLatchedStop` (a stop is written, or a turn cancelled, only in the step of the stop that found it running, so a stop that found nothing never touches a later turn), `TakesAtTurnEnd` (a queued prompt leaves the queue only at a turn's end, the oldest, one per turn, as the person's entry), `EffectsForOpen` (every entry of a turn is for the turn open before the step), `EndsAnsweredOrCut` (a turn ends by its done or by a cut with its notice), `TextKept` (gathered text goes into the tab before the turn's closing notice), `StartsSound` (a driver starts only when not held back, and with `--resume` the last session id), `NoSendWithoutCredential` (no prompt is written to a driver that reported it has no credential). Liveness: `Settles` (every prompt accepted is answered or cut, or waits while starting is held back or the server is stopping), under fairness for deadlines, the person's stop of a working turn (`StopWorking`: there is no deadline on one; a stop of a start is never assumed, so no prompt settles by being cancelled) and KILL.

Instances: `MCSupervisor.cfg`, two prompts, one waiting, two failed starts, one restart, one session id (26,302 states, 6 s on four workers); nightly, `MCSupervisorQueue.cfg`, three prompts, two waiting or queued, no restart (111,319 states, 33 s), and `MCSupervisorWide.cfg`, three prompts (each sent or queued), three failed starts, two restarts, two session ids (1,117,970 states, 4 min 11 s on eight workers, so capped at 900 s).

Reversed witnesses (`witnesses/WSup*.tla`), each rejected by TLC:

| Witness | The break | Rejected by |
|---|---|---|
| `WSupResendAfterCut` | an exit cuts the turn but puts its prompt back at the head of the waiting prompts | `AtMostOnce` |
| `WSupTurnReuse` | the next turn number is not durable before its send, so a restart reuses it | `TurnsIncrease` |
| `WSupStaleText` | text for a turn other than the open one is shown instead of ending the driver | `EffectsForOpen` |
| `WSupTwoDrivers` | a driver silent past the hello deadline is replaced without being ended | `OneDriver` |
| `WSupNoCutOnExit` | an exit leaves the turn open with no driver | `OneTurn` |
| `WSupStartLoop` | failed starts never stop the starting | `LimitHolds` |
| `WSupNoStopTimeout` | a stopped turn has no deadline | `Settles` |
| `WSupSendWhileOpen` | a prompt is written while a turn is open | `OneTurn` |
| `WSupTextLost` | text gathered before a mid-turn exit is dropped | `TextKept` |
| `WSupStartFresh` | a driver starts without `--resume` | `StartsSound` |
| `WSupSendNoCredential` | a driver that reported no credential is left idle after its turn's done, and given the next prompt | `NoSendWithoutCredential` |
| `WSupTakeWhileOpen` | a queued prompt is taken whenever nothing waits, even while a turn is open | `TakesAtTurnEnd` |
| `WSupTakeNewest` | at a turn's end the newest queued prompt is taken, not the oldest | `TakesAtTurnEnd` |
| `WSupTakeKeeps` | a queued prompt taken joins the waiting prompts but stays in the queue too | `QueueSound` |
| `WSupStopAgain` | a second stop is written for a turn whose stop is already out | `StopOnce` |
| `WSupStopStartIgnored` | a stop while the driver starts does nothing, so the turn about to begin begins anyway | `StopEndsWhatRuns` |
| `WSupStopLatched` | a stop that finds nothing running is kept, and stops the next turn to begin | `NoLatchedStop` |

The binding: `tests/server/supervisor_walk.cpp` (`supervisor_walk <seed> <steps> [--faulty-turn | --faulty-resume | --faulty-queue] <out.tla>`) runs the real `Supervisor` against a scripted driver chosen from the seed (good turns, stops, wrong turns, bad lines, exits, silent starts, credentials, errors including `no-credential`, server restarts and a shutdown) and the person's prompts sent and queued, keeping the queue the store would hold (each take a dequeue) and checking it against the supervisor's, with the clock moved to each deadline and, one millisecond before it, checked to do nothing. It records each event with the code's whole state after it; `SupervisorTrace.tla` takes the `Supervisor` action each record names, `TraceMatches` requires the spec's state to equal the code's, and every safety property is checked on the trace. Seeds 1 to 4 and 12 run per commit, 600 steps each (seeds 1, 3 and 12 stop a start; seed 12 then takes a queued prompt at the hello). The binding is shown to fail three times: `--faulty-turn` loses the next turn number at a restart, `--faulty-resume` the session id, and `--faulty-queue` the queue (on seed 12, whose walk restarts with prompts queued); TLC must report `TraceMatches` violated. The walk does not exercise what the spec leaves out: text past the entry limit (the code puts it in the tab in whole entries before the block ends), empty deltas (the code gathers nothing, so no entry), and the cut of a driver's error cause and fix to `kReportBytes` each. Nor does it see the process layer (`src/server/drivers.cpp`): commands are recorded only through the phase and stage they leave; the functional tests cover the processes.

## Glide.tla: the sprint board's scrolling

`Glide.tla` models `nexus::GlideScroller` (`src/scroll_glide.h`), which animates the board's scroll for the keys and the mouse wheel. The page's scroll is `pos` in `0..Max`; the band drawn past an end is `over`. The motion is `mode`: `idle`, a `coast` easing to `target` (exponential decay), a `hold` running while an arrow is down, or a `spring` taking the band back to an end (critically damped). Time is frames of `Dts` ticks; a coast or a spring that has not settled by its cap (`CoastTicks`, `SpringTicks`) lands where it was going. The curves are abstracted: each frame moves the page any amount the right way, up to the target, so a curve that overshoots or turns back is caught by the properties. The actions are an arrow pressed (`Press`), its OS repeat (ignored), its release (`Release`; the window losing focus is a release too, as Prax sends a key-up for every held key on resign), another key (`Key(d)`: a page, an end), a notch (`Wheel(d)`), a trackpad's scroll or a press of the pointer (`Interrupt`), the page's own script scrolling (`Page(y)`), the board leaving the screen (`Hide`), and `Frame(dt)`.

| Property | Kind | Says |
|---|---|---|
| `InRange` | invariant | the scroll is in its range, the band under the view's height |
| `BandAtEnds` | invariant | the band only at an end, on its outer side |
| `ShowsItself` | invariant | while a motion runs, the page shows what the scroller last set |
| `SpringAtEnd` | invariant | a spring stays at its end and never swings into the page |
| `HeldOnlyPressed`, `HoldIsHeld` | invariant | only an arrow held down holds |
| `EventsKeepPlace` | action | an event never moves the page itself: what it starts moves from where the page shows |
| `NeverBack` | action | a coast or a hold, and every event during one, never moves the page back against its way (a retarget never jumps back) |
| `PadStops` | action | a trackpad's scroll or a click stops the motion and the hold |
| `Settles` | liveness | once input stops, the page comes to rest and nothing is held |

`MCGlide` is the instance: a page of 3, lines of 1 and keys of 2 or 3, 15,668 states in about 2 s.

| Witness | Broken | Rejected by |
|---|---|---|
| `WGlideJumpBack` | a key restarts the motion from where it began | `NeverBack` |
| `WGlideClampTop` | no clamp at the top | `InRange` |
| `WGlidePadKeeps` | a trackpad's scroll leaves the motion running | `PadStops` |
| `WGlideLostKeyUp` | a release is lost, so the hold runs on | `Settles` |
| `WGlideSpringInside` | the spring overshoots into the page | `SpringAtEnd` |
| `WGlideSpringForever` | the spring never settles | `Settles` |

The binding: `tests/model/glide_walk.cpp` (`glide_walk <seed> <steps> [--faulty-keyup] <out.tla>`) drives the real `GlideScroller` over a fake page in whole px (2000 of scroll, 800 high, lines of 40) with a seeded walk of presses, repeats, releases, page and end keys, notches, trackpad scrolls, clicks, the page's own scrolls, hides and frames of 4 to 33 ms, in time it counts itself. `GlideTrace.tla` pins each step's next state to the record (scroll, band, motion, target, cap left, hold) and takes the `Glide` action it names; a step the spec cannot take is a deadlock. Five seeds of 1000 steps run per commit; `--faulty-keyup` walks a scroller that loses the arrow's release, and TLC must report the deadlock.

## Overlay.tla: when the overlay shows and hides

`Overlay.tla` is when the overlay shows and hides: `nexus::OverlayRules` in `src/overlay.h`, with the window that carries out its answers (`main.cpp` `carryOut`, through Prax) and the OS around it. Variables: `visible` (`prAppWindowIsVisible`), `active` (`prAppWindowIsActive`: the overlay has the keyboard), `keyed` (the rules' note that it has had the keyboard since it was last shown), `armed` (the key check after a show is pending, a Prax timer), `last`, the input just handled and the act answered, and `held`, whether nexus holds the window (`main.cpp` `a.window`): `carryOut` acts only on a window it holds. Inputs: `Hotkey`, `Escape`, `Observe` (a frame; a change of `visible` or `active` asks for one) and `KeyCheck`. The OS: a show gets the keyboard or not, `Lose` (Command-Tab, a click on another app), `Click` (a click on the overlay gives it back). Prax: `Open`, the window's open, which comes only with a frame of a shown window (a hidden window has no frames). `start()` creates the overlay window itself, so `Init` holds it from the start. `Spec` has weak fairness on `Observe` and `KeyCheck`: the frames and the check that a change asks for do come.

| Property | Kind | What it says |
|---|---|---|
| `TypeOK` | invariant | every variable has its type, and every act is one the window knows |
| `KeyedOnlyVisible` | invariant | only a shown overlay has had the keyboard |
| `UnkeyedIsChecked` | invariant | a shown overlay that has not had the keyboard always has a key check coming |
| `HotkeyShows` | action | the hotkey shows the overlay from every state but up with the keyboard (after Command-Tab the next press shows it, not merely re-activates it) |
| `HotkeyHides` | action | the hotkey hides an overlay that is up with the keyboard |
| `EscapeHides` | action | Escape hides it |
| `NoHideWhileActive` | action | a frame or the key check never hides an overlay that has the keyboard (an input method's window or the emoji picker, which leave it the keyboard, cannot hide it) |
| `NoHideBeforeKey` | action | a frame never hides an overlay that has not had the keyboard since it was shown |
| `LossHides` | action | once it has had the keyboard, the frame that sees it lost hides it |
| `RefusedShowHides` | action | the key check hides an overlay shown without the keyboard |
| `NeverStranded` | temporal | a shown overlay without the keyboard is in time hidden or has it again |

`Overlay.cfg` checks every property on the whole state space (19 states). Each property has a witness in `witnesses/` (`WOverlay*`): `WOverlayActName` (Escape answers an unknown act: `TypeOK`), `WOverlayEscapeKeepsKeyed` (`KeyedOnlyVisible`), `WOverlayShowUnchecked` (a show arms no check: `UnkeyedIsChecked`), `WOverlayHotkeyHidesUnkeyed` (`HotkeyShows`), `WOverlayHotkeyOnlyShows` (`HotkeyHides`), `WOverlayEscapeIgnored` (`EscapeHides`), `WOverlayHideWhileActive` (`NoHideWhileActive`), `WOverlayHideBeforeKey` (`NoHideBeforeKey`), `WOverlayLossKept` (`LossHides`), `WOverlayRefusedKept` (`RefusedShowHides`), `WOverlayStranded` (neither a frame nor the check hides: `NeverStranded`, expected as a temporal violation) and `WOverlayHeldAfterOpen` (nexus holds the window only from its open, as the first lib/app overlay did, so a window that starts hidden is never held and the hotkey shows nothing: `HotkeyShows`).

The binding: `tests/model/overlay_walk.cpp` (`overlay_walk <seed> <steps> [--faulty-hide-unkeyed | --faulty-hotkey-hides] <out.tla>`) drives the real rules with a seeded random walk over a stand-in window that carries out their answers as `carryOut` does, and writes `OverlayTraceData.tla`. `OverlayTrace.tla` takes the `Overlay` action each record names, with the OS's answer to a show from the record, and `TraceMatches` requires the spec's state and act to equal the code's; every `Overlay` safety property is checked on the trace. Three seeds of 1000 steps run per commit; `--faulty-hide-unkeyed` (a frame hides any shown overlay without the keyboard) and `--faulty-hotkey-hides` (the hotkey hides any shown overlay) must each be rejected with `TraceMatches` violated. `held` is not in the walk: its stand-in window exists from the start, as `start()`'s does, and the trace check requires `held` throughout; that `start()` creates the window is read from the code, not traced.

## Running the checks

TLC needs Java 11 or newer and `tla2tools.jar`. The tests find the jar through `TLA2TOOLS_JAR`, and java through `JAVA` or `PATH`. Without `TLA2TOOLS_JAR` every `model` test fails and says what to set.

```sh
cmake -S . -B build
cmake --build build --target model_checks -j4
ctest --test-dir build -L unit
TLA2TOOLS_JAR=/path/to/tla2tools.jar ctest --test-dir build -L model -j "$(sysctl -n hw.ncpu)"
```

`-L model` matches two labels: `model` runs four instances (`MCWindowSprint`, `MCWindowEmpty`, `MCWindowNoSprint`, `MCWindow3Sprint`), the forty-eight witnesses, nine code traces (with the sprint tab first: seeds 1 to 5 on three friends, seed 6 on two friends and seed 8 on one, each 1000 steps, and seed 7 with no friends; without it: seed 9 on three friends), and the ten faulty traces; for the overlay, `Overlay.cfg`, its twelve witnesses, three code traces and two faulty traces; `model_nightly` runs the three largest instances, `MCWindow`, `MCWindow3` and `MCWindowLong`. CI's per-commit job runs `-L '^model$'` and the nightly workflow `-L '^model_nightly$'`: every property is still checked on every commit, on the smaller instances. Each TLC run gets its own directory under `build/tests/model/tlc/<test>/`, a copy of the models, its own `java.io.tmpdir`, two workers (eight on every instance but `MCWindowEmpty`, which ctest counts as eight of its `-j` slots) and a 60 second cap (300 on the nightly instances); its log is `tlc.log` there, and a trace run leaves its `WindowTraceData.tla` beside it.

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
