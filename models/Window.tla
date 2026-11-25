------------------------------- MODULE Window -------------------------------
(***************************************************************************)
(* The state the nexus window owns (src/model.h, nexus::Model): its tabs, *)
(* the order they stand in, the active tab, and for each AI its       *)
(* transcript, its unsent draft and its queue of prompts held for later.  *)
(* The queue is apart from the transcript: it is the state a server will  *)
(* hold.                                                                   *)
(*                                                                         *)
(* A tab is the sprint tab, an AI's or a terminal's. Each tab has an   *)
(* id that never changes and is never reused: an AI's at the start is  *)
(* its place in the constructor's list, from 0; the sprint tab's is N,    *)
(* the number of those AIs (SprintId); a terminal's, and an AI's   *)
(* added later, is `nextId` when it is added, and `nextId` then grows.    *)
(* The sprint tab exists when the constant Sprint is TRUE, as it always   *)
(* does in the window (main.cpp); FALSE is the model's form without it,   *)
(* which its tests and the older witnesses use. The sprint tab is made at *)
(* the start and never closed;                                             *)
(* terminal tabs are added at the end and closed by position. The sprint  *)
(* tab and a terminal have no transcript, draft or queue: the per-AI  *)
(* state is indexed by AI, so it moves with the tab.                   *)
(*                                                                         *)
(* The strip is in zones: the sprint tab alone in zone 0, then every      *)
(* AI's tab, then every terminal's, and a move stays inside its zone. *)
(* Zone(id) is a number, and the zones stand in ascending order            *)
(* (ZonesOrdered). So the sprint tab stays first: nothing moves into its  *)
(* zone, and it moves only onto itself.                                    *)
(*                                                                         *)
(* A tab's position is its place in the strip, from 0, as in the code.    *)
(* `order` maps each position to the id of the tab there. `active` is a  *)
(* position, as in the code, and moves and closes carry it with the same  *)
(* arithmetic.                                                             *)
(*                                                                         *)
(* Actions are the operations of nexus::Model. Each takes the arguments   *)
(* the caller passes, positions in range and out of it and drafts that    *)
(* are all whitespace; a refused operation changes no model state. One    *)
(* operation takes from a queue, take_queued: its oldest prompt leaves,   *)
(* when the server delivered it (the window's mirror calls it, Link.tla). *)
(*                                                                         *)
(* `history` is the selection history: the ids of the tabs selected, most  *)
(* recent last, each once. The tab active at the start is its first entry; *)
(* select and add put their tab's id at the end (moving it there if it was *)
(* in it already), and close takes the closed tab's id out. Closing the    *)
(* active tab makes the last id left in it active: the tab selected before *)
(* it, of those still open. With none left, the rule before it: the tab   *)
(* now at the closed one's position, or the last. A close (or an AI's   *)
(* removal) that leaves the history empty with tabs still open enters the *)
(* tab that then shows (ClosedHistory), so the history always ends at the *)
(* active tab (HistoryEndsActive): an AI arriving unselected can leave *)
(* a terminal the history's only id. History is per run: the code saves   *)
(* none of it.                                                             *)
(*                                                                         *)
(* AIs come and go with the server's AI list (src/sync/client.h   *)
(* applyAIs): `AIs` maps each AI tab's id to the AI there, *)
(* and Ids is its domain, the AI tabs that stand. add_ai puts a   *)
(* AI's tab, with the next id, at the end of the AIs' zone, empty, *)
(* refused for an empty name or one a tab already has; the active tab     *)
(* stays the active tab, and the history is untouched unless the strip    *)
(* was empty. remove_ai takes an AI's tab out by position, as      *)
(* close_tab takes a terminal's: its id leaves the history, the tab       *)
(* selected before it shows if it was active, and its transcript, draft   *)
(* and queue go with it (the session stays on the server; an AI added  *)
(* again is a new tab with a new id). Spec has neither; SpecAIsWith   *)
(* adds both, with the AIs add_ai is tried with.                   *)
(*                                                                         *)
(* `last` is not model state. It is a history variable: the operation     *)
(* just performed, its arguments and the value it returned. It lets the   *)
(* per-operation guarantees below be stated as action properties, and it  *)
(* is what a code trace's return values are bound to (WindowTrace.tla).   *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    AIList,     \* <<[name |-> .., model |-> ..], ...>>, the constructor's argument
    Idx,            \* the positions the operations are tried with, in range and out of it
    Drafts,         \* the draft texts set_draft is tried with
    Blank,          \* the members of Drafts that are all whitespace (the empty string among them)
    AppendEntries,  \* the entries append is tried with
    Sprint          \* TRUE: the strip starts with the sprint tab (the window's model always does)

ASSUME Blank \subseteq Drafts /\ "" \in Blank /\ Sprint \in BOOLEAN

VARIABLES ais, order, active, transcript, draft, queue, nextId, history, last

model == <<ais, order, active, transcript, draft, queue, nextId, history>>
vars == <<ais, order, active, transcript, draft, queue, nextId, history, last>>

N == Len(AIList)
Ids == DOMAIN ais       \* the AI tabs that stand, by id: at the start their places in AIList
SprintId == N               \* the sprint tab's id, when there is one: the first after the AIs'
Pinned == IF Sprint THEN 1 ELSE 0   \* the tabs before the AIs at the start
Tabs == DOMAIN order        \* positions in the strip
Count == Cardinality(Tabs)  \* how many tabs stand in it
Kinds == {"Person", "AI", "Notice", "Tool"}
Texts == Drafts \cup {e.text : e \in AppendEntries}
Entry == [kind : Kinds, text : Texts]
NoEntry == [kind |-> "None", text |-> ""]
NoAI == [name |-> "", model |-> ""]
Ops == {"init", "select", "set_draft", "send", "queue", "take", "append", "move", "add", "close", "add_ai",
        "remove_ai"}

\* a tab's kind, by its id: the AIs' ids are theirs, SprintId the sprint tab's if there is one, every later id a
\* terminal's
TabKind(id) == IF id \in Ids THEN "AI" ELSE IF Sprint /\ id = SprintId THEN "Sprint" ELSE "Terminal"

\* the zone a tab stands in, by its id: the sprint tab 0, AIs 1, terminals 2
Zone(id) == CASE TabKind(id) = "Sprint" -> 0 [] TabKind(id) = "AI" -> 1 [] OTHER -> 2

Did(op, i, t, e, ok) == [op |-> op, i |-> i, j |-> 0, text |-> t, entry |-> e, ok |-> ok, id |-> 0,
                         ai |-> NoAI]
DidMove(from, to, ok) == [op |-> "move", i |-> from, j |-> to, text |-> "", entry |-> NoEntry, ok |-> ok, id |-> 0,
                          ai |-> NoAI]
DidAdd(id) == [op |-> "add", i |-> 0, j |-> 0, text |-> "", entry |-> NoEntry, ok |-> TRUE, id |-> id,
               ai |-> NoAI]
\* add_ai (i 0) and remove_ai (i the position): the AI, and the tab's id when it returned true
DidAI(op, i, f, ok, id) == [op |-> op, i |-> i, j |-> 0, text |-> "", entry |-> NoEntry, ok |-> ok, id |-> id,
                                ai |-> f]

\* the id of the tab that stands at position i
At(i) == order[i]

\* i is the position of an AI's tab: only those hold a transcript, draft and queue
AITab(i) == i \in Tabs /\ At(i) \in Ids

\* send and queue take a draft only on an AI's tab, and only when it is not all whitespace
Sendable(i) == AITab(i) /\ draft[At(i)] \notin Blank

\* The order after the tab at `from` is taken out and put back in at `to`: the tabs
\* between shift by one toward the place it left. The code's std::rotate.
Moved(o, from, to) ==
    [p \in DOMAIN o |->
        IF p = to THEN o[from]
        ELSE IF from < to /\ from <= p /\ p < to THEN o[p + 1]
        ELSE IF to < from /\ to < p /\ p <= from THEN o[p - 1]
        ELSE o[p]]

\* Where the tab at position a stands after that move: the code's arithmetic on active_.
MovedIndex(a, from, to) ==
    IF a = from THEN to
    ELSE IF from < a /\ a <= to THEN a - 1
    ELSE IF to <= a /\ a < from THEN a + 1
    ELSE a

\* The order after the tab at i is taken out: the tabs after it shift left. The code's erase.
Removed(o, i) == [p \in 0 .. (Cardinality(DOMAIN o) - 2) |-> IF p < i THEN o[p] ELSE o[p + 1]]

\* The active position after the tab at i is closed, of n tabs before: the code's arithmetic on active_.
ClosedIndex(a, i, n) ==
    IF a > i THEN a - 1
    ELSE IF a = i /\ i = n - 1 THEN (IF n = 1 THEN 0 ELSE i - 1)
    ELSE a

\* The selection history without id f: the code's std::remove.
Forget(h, f) == SelectSeq(h, LAMBDA g : g /= f)

\* The selection history with id f at its end, and nowhere else.
Touch(h, f) == Append(Forget(h, f), f)

\* The position of the tab with id f in the order o (f stands in it).
PosOf(o, f) == CHOOSE p \in DOMAIN o : o[p] = f

\* The active position after the tab at i is closed, of the order o, with the history h that is left: the last id in h
\* if the closed tab was active and h holds one, else ClosedIndex. The code's close_tab.
ClosedActive(a, i, o, h) ==
    IF a = i /\ h /= <<>> THEN PosOf(Removed(o, i), h[Len(h)]) ELSE ClosedIndex(a, i, Cardinality(DOMAIN o))

\* The history after the tab at i of the order o is closed, with the history h that is left once its id is forgotten:
\* h, but for an empty h with tabs left, where the tab that is now active (ClosedActive) is the history. A tab nobody
\* selected, such as an AI's that arrived while a terminal was active, is then the one that shows, and it is entered
\* so that the history still ends at the active tab. The code's remove_at.
ClosedHistory(a, i, o, h) ==
    IF h = <<>> /\ Cardinality(DOMAIN o) > 1 THEN <<Removed(o, i)[ClosedActive(a, i, o, h)]>> ELSE h

\* An AI standing has the name.
Standing(name) == \E g \in Ids : ais[g].name = name

\* The position just past the AIs' zone in the order o: the sprint tab's and the AIs' tabs stand before it.
AIEnd(o) == Cardinality({p \in DOMAIN o : Zone(o[p]) <= 1})

\* The function f on the ids `keep` only, or with id n mapped to v as well.
Keep(f, keep) == [g \in keep |-> f[g]]
With(f, n, v) == [g \in DOMAIN f \cup {n} |-> IF g = n THEN v ELSE f[g]]

-----------------------------------------------------------------------------

\* The sprint tab first if there is one, then the AIs in their order; the first tab active, and the first entry of
\* the selection history.
Init ==
    /\ ais = [f \in 0 .. (N - 1) |-> AIList[f + 1]]
    /\ order = [p \in 0 .. (N + Pinned - 1) |-> IF Sprint /\ p = 0 THEN SprintId ELSE p - Pinned]
    /\ active = 0
    /\ transcript = [f \in 0 .. (N - 1) |-> <<>>]
    /\ draft = [f \in 0 .. (N - 1) |-> ""]
    /\ queue = [f \in 0 .. (N - 1) |-> <<>>]
    /\ nextId = N + Pinned
    /\ history = IF Sprint THEN <<SprintId>> ELSE IF N > 0 THEN <<0>> ELSE <<>>
    /\ last = Did("init", 0, "", NoEntry, TRUE)

\* select_tab(i): its tab active, and its id at the end of the history
SelectTab(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ history' = Touch(history, At(i))
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* set_draft(i, t)
SetDraft(i, t) ==
    IF AITab(i)
    THEN /\ draft' = [draft EXCEPT ![At(i)] = t]
         /\ last' = Did("set_draft", i, t, NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, transcript, queue, nextId, history>>
    ELSE /\ last' = Did("set_draft", i, t, NoEntry, FALSE)
         /\ UNCHANGED model

\* send(i): the draft becomes a Person entry at the end of that tab's transcript
Send(i) ==
    IF Sendable(i)
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, [kind |-> "Person", text |-> draft[At(i)]])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, queue, nextId, history>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* queue(i): the draft goes to the end of that tab's queue
Queue(i) ==
    IF Sendable(i)
    THEN /\ queue' = [queue EXCEPT ![At(i)] = Append(@, draft[At(i)])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, transcript, nextId, history>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* take_queued(i): the oldest prompt of that tab's queue leaves it
TakeQueued(i) ==
    IF AITab(i) /\ queue[At(i)] /= <<>>
    THEN /\ queue' = [queue EXCEPT ![At(i)] = Tail(@)]
         /\ last' = Did("take", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, transcript, draft, nextId, history>>
    ELSE /\ last' = Did("take", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* append(i, e)
AppendEntry(i, e) ==
    IF AITab(i)
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, e)]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<ais, order, active, draft, queue, nextId, history>>
    ELSE /\ last' = Did("append", i, "", e, FALSE)
         /\ UNCHANGED model

\* move_tab(from, to): the tab at `from` ends at `to`, and active follows its tab. Both ends in one zone, so the
\* tabs that shift are of that zone too.
MoveTab(from, to) ==
    IF from \in Tabs /\ to \in Tabs /\ Zone(At(from)) = Zone(At(to))
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId, history>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

\* add_terminal(): a terminal's tab at the end, active, with the next id, which goes to the end of the history
AddTerminal ==
    /\ order' = [p \in 0 .. Count |-> IF p < Count THEN order[p] ELSE nextId]
    /\ active' = Count
    /\ nextId' = nextId + 1
    /\ history' = Touch(history, nextId)
    /\ last' = DidAdd(nextId)
    /\ UNCHANGED <<ais, transcript, draft, queue>>

\* close_tab(i): only a terminal's tab closes; its id leaves the history, and if it was active the tab selected before
\* it is (ClosedActive)
CloseTab(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ history' = ClosedHistory(active, i, order, Forget(history, At(i)))
         /\ active' = ClosedActive(active, i, order, Forget(history, At(i)))
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* add_ai(f): a tab for AI f, with the next id, at the end of the AIs' zone (before every terminal), empty.
\* The active tab stays the active tab, wherever it now stands; the history is untouched, but for an empty strip, where
\* the new tab is the active one and so its id is the history. Refused for an empty name, or a name an AI's tab
\* already has.
AddAI(f) ==
    IF f.name /= "" /\ ~Standing(f.name)
    THEN LET at == AIEnd(order) IN
         /\ order' = [p \in 0 .. Count |-> IF p < at THEN order[p] ELSE IF p = at THEN nextId ELSE order[p - 1]]
         /\ active' = IF Count > 0 /\ active >= at THEN active + 1 ELSE active
         /\ ais' = With(ais, nextId, f)
         /\ transcript' = With(transcript, nextId, <<>>)
         /\ draft' = With(draft, nextId, "")
         /\ queue' = With(queue, nextId, <<>>)
         /\ nextId' = nextId + 1
         /\ history' = IF Count = 0 THEN <<nextId>> ELSE history
         /\ last' = DidAI("add_ai", 0, f, TRUE, nextId)
    ELSE /\ last' = DidAI("add_ai", 0, f, FALSE, 0)
         /\ UNCHANGED model

\* remove_ai(i): only an AI's tab is removed this way, as close_tab removes a terminal's: its id leaves the
\* history, and if it was active the tab selected before it is (ClosedActive). Its transcript, draft and queue go with
\* it.
RemoveAI(i) ==
    IF AITab(i)
    THEN LET f == At(i) IN
         /\ order' = Removed(order, i)
         /\ history' = ClosedHistory(active, i, order, Forget(history, f))
         /\ active' = ClosedActive(active, i, order, Forget(history, f))
         /\ ais' = Keep(ais, Ids \ {f})
         /\ transcript' = Keep(transcript, Ids \ {f})
         /\ draft' = Keep(draft, Ids \ {f})
         /\ queue' = Keep(queue, Ids \ {f})
         /\ last' = DidAI("remove_ai", i, ais[f], TRUE, f)
         /\ UNCHANGED nextId
    ELSE /\ last' = DidAI("remove_ai", i, NoAI, FALSE, 0)
         /\ UNCHANGED model

\* The next-state relation over any set of the eight actions. The witnesses (witnesses/*.tla) pass a broken action in
\* place of one of these. The first six never change nextId, and of them only select changes the history: the conjuncts
\* say so for a broken one that leaves them out.
NextWithAll(Sel(_), SetD(_, _), Snd(_), Que(_), App(_, _), Mov(_, _), Add, Cls(_)) ==
    \/ /\ \E i \in Idx :
            \/ Sel(i)
            \/ \E t \in Drafts : SetD(i, t) /\ UNCHANGED history
            \/ Snd(i) /\ UNCHANGED history
            \/ Que(i) /\ UNCHANGED history
            \/ \E e \in AppendEntries : App(i, e) /\ UNCHANGED history
            \/ \E j \in Idx : Mov(i, j) /\ UNCHANGED history
       /\ UNCHANGED nextId
    \/ Add
    \/ \E i \in Idx : Cls(i)

\* With the real add and close: the form the witnesses of the first six actions use.
NextWith(Sel(_), SetD(_, _), Snd(_), Que(_), App(_, _), Mov(_, _)) ==
    NextWithAll(Sel, SetD, Snd, Que, App, Mov, AddTerminal, CloseTab)

\* With take_queued as well: the form its witnesses use, passing a broken take.
NextTake(Tk(_)) ==
    \/ NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab)
    \/ \E i \in Idx : Tk(i)

Next == NextTake(TakeQueued)

Spec == Init /\ [][Next]_vars

\* With the AIs coming and going: add_ai tried with each AI of `Arrivals`, remove_ai at every position.
\* The witnesses of the two pass a broken one in place of either.
NextAIOps(AddF(_), RemF(_), Arrivals) ==
    \/ Next
    \/ \E f \in Arrivals : AddF(f)
    \/ \E i \in Idx : RemF(i)

NextAIsWith(Arrivals) == NextAIOps(AddAI, RemoveAI, Arrivals)

SpecAIsWith(Arrivals) == Init /\ [][NextAIsWith(Arrivals)]_vars

-----------------------------------------------------------------------------
(* State invariants *)

TypeOK ==
    /\ ais \in [Ids -> [name : STRING, model : STRING]]
    /\ Ids \subseteq 0 .. (nextId - 1)
    /\ nextId \in Int /\ nextId >= N + Pinned
    /\ Tabs = 0 .. (Count - 1)
    /\ \A p \in Tabs : order[p] \in Int
    /\ active \in Int
    /\ transcript \in [Ids -> Seq(Entry)]
    /\ draft \in [Ids -> Drafts]
    /\ queue \in [Ids -> Seq(Drafts \ Blank)]
    /\ history \in Seq(Int)
    /\ last.op \in Ops
    /\ last.i \in Idx \cup {0}
    /\ last.j \in Idx \cup {0}
    /\ last.text \in Drafts
    /\ last.entry \in AppendEntries \cup {NoEntry}
    /\ last.ok \in BOOLEAN
    /\ last.id \in Int
    /\ last.ai \in [name : STRING, model : STRING]

\* active is always a tab, or 0 when there are no tabs
ActiveValid == IF Count = 0 THEN active = 0 ELSE active \in Tabs

\* every AI has a tab: no reordering or close loses one
NoTabLost == \A f \in Ids : \E p \in Tabs : order[p] = f

\* no two AI tabs are of one AI: add_ai never makes a second tab for a name that has one
NamesOnce == \A f, g \in Ids : f /= g => ais[f].name /= ais[g].name

\* no id stands twice: no reordering duplicates a tab, and no add reuses an id that stands
NoTabDuplicated == \A p, q \in Tabs : p /= q => order[p] /= order[q]

\* every id that stands was handed out: below nextId
IdsHandedOut == \A p \in Tabs : 0 <= order[p] /\ order[p] < nextId

\* the zones stand in ascending order: the sprint tab before every AI's, every AI's before every terminal's
ZonesOrdered == \A p, q \in Tabs : p < q => Zone(order[p]) <= Zone(order[q])

\* with the sprint tab, it is always there, always first, and always has its one id
SprintFirst == Sprint => (0 \in Tabs /\ order[0] = SprintId)

\* the history holds only ids of tabs that stand: never a closed tab's, never one not handed out
HistoryOpen == \A k \in 1 .. Len(history) : \E p \in Tabs : order[p] = history[k]

\* the history holds each id once
HistoryOnce == \A k, m \in 1 .. Len(history) : k /= m => history[k] /= history[m]

\* the history's last id is the active tab's, whenever there is a tab
HistoryEndsActive == Count > 0 => (history /= <<>> /\ history[Len(history)] = order[active])

-----------------------------------------------------------------------------
(* Action properties: what each operation does to the state, checked on   *)
(* every step whatever the action that took it. Each is stated per        *)
(* ai: the operation on position i acts on At(i), the AI there.   *)

Others(f) == Ids \ {f}

\* select_tab returns true exactly when i is a tab, and changes only active, to i
SelectExact ==
    [][last'.op = "select" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ active' = (IF last'.ok THEN last'.i ELSE active)
        /\ UNCHANGED <<ais, order, transcript, draft, queue, nextId>>]_vars

\* set_draft returns true exactly when i is an AI's tab, and changes only that AI's draft, to the text given
SetDraftExact ==
    [][last'.op = "set_draft" =>
        /\ last'.ok = AITab(last'.i)
        /\ draft' = (IF last'.ok THEN [draft EXCEPT ![At(last'.i)] = last'.text] ELSE draft)
        /\ UNCHANGED <<ais, order, active, transcript, queue, nextId>>]_vars

\* send and queue return true exactly when i is an AI's tab whose draft is not all whitespace
SendResult ==
    [][last'.op \in {"send", "queue"} =>
        last'.ok = (AITab(last'.i) /\ draft[At(last'.i)] \notin Blank)]_vars

\* a terminal's tab takes no text: set_draft, send, queue and append addressed to one return false
TerminalTakesNoText ==
    [][(last'.op \in {"set_draft", "send", "queue", "append"} /\ last'.i \in Tabs /\
        TabKind(At(last'.i)) = "Terminal") => ~last'.ok]_vars

\* the sprint tab takes no text: set_draft, send, queue and append addressed to it return false
SprintTakesNoText ==
    [][(last'.op \in {"set_draft", "send", "queue", "append"} /\ last'.i \in Tabs /\
        TabKind(At(last'.i)) = "Sprint") => ~last'.ok]_vars

\* the sprint tab never moves: a move from or to its position leaves the order and the active tab as they were
SprintNeverMoves ==
    [][(Sprint /\ last'.op = "move" /\ (last'.i = 0 \/ last'.j = 0)) => UNCHANGED <<order, active>>]_vars

\* the sprint tab never closes: close addressed to it returns false
SprintNeverCloses ==
    [][(last'.op = "close" /\ last'.i \in Tabs /\ TabKind(At(last'.i)) = "Sprint") => ~last'.ok]_vars

\* a send that returns true appends exactly one Person entry, with the draft's text, to that AI's transcript
SendAppendsOne ==
    [][(last'.op = "send" /\ last'.ok) =>
        LET f == At(last'.i) IN
        transcript'[f] = Append(transcript[f], [kind |-> "Person", text |-> draft[f]])]_vars

\* a queue that returns true appends exactly the draft's text to that AI's queue
QueueAppendsOne ==
    [][(last'.op = "queue" /\ last'.ok) =>
        LET f == At(last'.i) IN queue'[f] = Append(queue[f], draft[f])]_vars

\* a queue never changes any transcript
QueueLeavesTranscripts ==
    [][last'.op = "queue" => UNCHANGED transcript]_vars

\* only queue and take change a queue: send, select, set_draft, append, move, add and close never do (add_ai and
\* remove_ai add or take out an AI's whole queue: AIOpsKeepOthers)
OnlyQueueTouchesQueues ==
    [][last'.op \notin {"queue", "take", "add_ai", "remove_ai"} => UNCHANGED queue]_vars

\* take returns true exactly when i is an AI's tab with a queued prompt; then exactly that queue's oldest prompt
\* leaves it, and nothing else changes
TakeExact ==
    [][last'.op = "take" =>
        /\ last'.ok = (AITab(last'.i) /\ queue[At(last'.i)] /= <<>>)
        /\ queue' = (IF last'.ok THEN [queue EXCEPT ![At(last'.i)] = Tail(@)] ELSE queue)
        /\ UNCHANGED <<ais, order, active, transcript, draft, nextId, history>>]_vars

\* a send or queue that returns true clears that AI's draft and no other
SendClearsOnlyItsDraft ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        draft' = [draft EXCEPT ![At(last'.i)] = ""]]_vars

\* a send or queue never touches another AI, the order, the active tab, the ids or the AIs
SendTouchesNoOtherTab ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        /\ \A g \in Others(At(last'.i)) :
            transcript'[g] = transcript[g] /\ draft'[g] = draft[g] /\ queue'[g] = queue[g]
        /\ UNCHANGED <<ais, order, active, nextId>>]_vars

\* append returns true exactly when i is an AI's tab, appends exactly that entry to that AI and changes
\* nothing else
AppendExact ==
    [][last'.op = "append" =>
        /\ last'.ok = AITab(last'.i)
        /\ transcript' = (IF last'.ok
                          THEN [transcript EXCEPT ![At(last'.i)] = Append(@, last'.entry)]
                          ELSE transcript)
        /\ UNCHANGED <<ais, order, active, draft, queue, nextId>>]_vars

\* the order as a sequence of ids, from the first position, leaving out id f
Without(o, f) == SelectSeq([k \in 1 .. Cardinality(DOMAIN o) |-> o[k - 1]], LAMBDA g : g /= f)

\* move returns true exactly when both positions are tabs of one zone; then the tab that stood at `from`
\* stands at `to`, and every other tab keeps its place relative to the rest
MovePlaces ==
    [][last'.op = "move" =>
        /\ last'.ok = (last'.i \in Tabs /\ last'.j \in Tabs /\ Zone(At(last'.i)) = Zone(At(last'.j)))
        /\ last'.ok =>
            /\ order'[last'.j] = order[last'.i]
            /\ Without(order', order[last'.i]) = Without(order, order[last'.i])]_vars

\* a move changes no AI's transcript, draft or queue, nor the ids or the AIs: they go with the tab
MoveKeepsTabs ==
    [][last'.op = "move" => UNCHANGED <<ais, transcript, draft, queue, nextId>>]_vars

\* only move reorders the tabs, add puts one at the end, add_ai one at the AIs' end, and close and remove_ai
\* take one out: no other operation changes the order
OnlyMoveReorders ==
    [][last'.op \notin {"move", "add", "close", "add_ai", "remove_ai"} => UNCHANGED order]_vars

\* every operation but select, add, close and remove_ai keeps the same tab active, wherever it now stands
ActiveAIKept ==
    [][(Count > 0 /\ last'.op \notin {"select", "add", "close", "remove_ai"}) => order'[active'] = order[active]]_vars

\* add always succeeds: one terminal's tab, with the id it returns, at the end, active; every other tab where it was,
\* and no AI's state touched
AddAppends ==
    [][last'.op = "add" =>
        /\ last'.ok
        /\ DOMAIN order' = 0 .. Count
        /\ \A p \in Tabs : order'[p] = order[p]
        /\ order'[Count] = last'.id
        /\ TabKind(last'.id) = "Terminal"
        /\ active' = Count
        /\ UNCHANGED <<ais, transcript, draft, queue>>]_vars

\* the id add returns was never handed out before (every one handed out is below nextId), and after it none below
\* or at it will be
AddFreshId ==
    [][last'.op = "add" => last'.id >= nextId /\ nextId' > last'.id]_vars

\* nextId never goes back, so no id is handed out twice
NextIdGrows == [][nextId' >= nextId]_vars

\* close returns true exactly when i is a terminal's tab; then exactly that tab is gone and the tabs after it
\* shift left by one
CloseRemovesExactly ==
    [][last'.op = "close" =>
        /\ last'.ok = (last'.i \in Tabs /\ TabKind(At(last'.i)) = "Terminal")
        /\ last'.ok =>
            /\ DOMAIN order' = 0 .. (Count - 2)
            /\ \A p \in DOMAIN order' : order'[p] = IF p < last'.i THEN order[p] ELSE order[p + 1]]_vars

\* a close changes no AI's transcript, draft or queue, nor the ids or the AIs
CloseKeepsAIs ==
    [][last'.op = "close" => UNCHANGED <<ais, transcript, draft, queue, nextId>>]_vars

\* after a close, the same tab is active if another was; if the closed tab was active, the tab selected most recently
\* before it, of those still open: the last id in the history once the closed tab's is taken out. With none there, the
\* tab now at its position is, or the last tab if it was the last, or none (0) if no tabs remain
CloseActiveRule ==
    [][(last'.op = "close" /\ last'.ok) =>
        LET n == Cardinality(DOMAIN order')
            h == Forget(history, order[last'.i]) IN
        IF active /= last'.i
        THEN order'[active'] = order[active]
        ELSE IF h /= <<>>
        THEN order'[active'] = h[Len(h)]
        ELSE active' = (IF n = 0 THEN 0 ELSE IF last'.i < n THEN last'.i ELSE n - 1)]_vars

\* a select that returns true, and every add, puts its tab's id at the end of the history and takes it out of where it
\* stood; the other ids keep their order
SelectMovesToEnd ==
    [][((last'.op = "select" /\ last'.ok) \/ last'.op = "add") =>
        LET f == IF last'.op = "add" THEN last'.id ELSE order[last'.i] IN
        /\ history' /= <<>>
        /\ history'[Len(history')] = f
        /\ SubSeq(history', 1, Len(history') - 1) = Forget(history, f)]_vars

\* a close that returns true takes the closed tab's id out of the history, and nothing else; but when that leaves the
\* history empty with tabs still open, the history is the tab now active, alone
LeftHistory(h) ==
    IF h = <<>> /\ Cardinality(DOMAIN order') > 0 THEN <<order'[active']>> ELSE h
CloseForgets ==
    [][(last'.op = "close" /\ last'.ok) => history' = LeftHistory(Forget(history, order[last'.i]))]_vars

\* an operation that returns false changes no model state
RefusedChangesNothing ==
    [][~last'.ok => UNCHANGED model]_vars

\* s' extends s: its entries are s's, in s's order, then any new ones
Extends(s2, s1) == Len(s2) >= Len(s1) /\ SubSeq(s2, 1, Len(s1)) = s1

\* transcripts only grow by appending: existing entries never change, move or go, for as long as their tab stands
TranscriptsOnlyGrow ==
    [][\A f \in Ids \cap Ids' : Extends(transcript'[f], transcript[f])]_vars

\* queues only grow by appending, but for take: queued prompts never change or move, and go only from the front
QueuesOnlyGrow ==
    [][last'.op /= "take" => \A f \in Ids \cap Ids' : Extends(queue'[f], queue[f])]_vars

\* the AIs change only by add_ai and remove_ai: no other operation renames, adds or drops one
AIsFixed == [][last'.op \notin {"add_ai", "remove_ai"} => UNCHANGED ais]_vars

\* add_ai returns true exactly when the AI's name is not empty and no AI's tab has it. Then its tab, with
\* the id it returns (never handed out before), stands at the end of the AIs' zone, holding nothing; every other tab
\* keeps its place relative to the rest; and the tab that was active is active still
AddAIExact ==
    [][last'.op = "add_ai" =>
        /\ last'.ok = (last'.ai.name /= "" /\ ~Standing(last'.ai.name))
        /\ last'.ok =>
            /\ last'.id = nextId /\ nextId' = nextId + 1
            /\ Ids' = Ids \cup {last'.id}
            /\ ais'[last'.id] = last'.ai
            /\ order'[AIEnd(order)] = last'.id
            /\ Without(order', last'.id) = Without(order, last'.id)
            /\ transcript'[last'.id] = <<>> /\ draft'[last'.id] = "" /\ queue'[last'.id] = <<>>
            /\ Count > 0 => (order'[active'] = order[active] /\ history' = history)]_vars

\* remove_ai returns true exactly when i is an AI's tab; then exactly that tab is gone, the tabs after it shift
\* left by one, and that AI is gone with it
RemoveAIExact ==
    [][last'.op = "remove_ai" =>
        /\ last'.ok = AITab(last'.i)
        /\ last'.ok =>
            /\ last'.id = order[last'.i]
            /\ Ids' = Ids \ {last'.id}
            /\ DOMAIN order' = 0 .. (Count - 2)
            /\ \A p \in DOMAIN order' : order'[p] = IF p < last'.i THEN order[p] ELSE order[p + 1]
            /\ UNCHANGED nextId]_vars

\* add_ai and remove_ai change no other AI: its name, transcript, draft and queue are as they were
AIOpsKeepOthers ==
    [][last'.op \in {"add_ai", "remove_ai"} =>
        \A f \in Ids \cap Ids' :
            ais'[f] = ais[f] /\ transcript'[f] = transcript[f] /\ draft'[f] = draft[f] /\ queue'[f] = queue[f]]_vars

\* a removed AI's tab leaves the history, and nothing else does; as for close, a history left empty with tabs open
\* is the tab now active, alone
RemoveForgets ==
    [][(last'.op = "remove_ai" /\ last'.ok) => history' = LeftHistory(Forget(history, order[last'.i]))]_vars

\* after a remove_ai, the same tab is active if another was; if the removed tab was active, the tab selected most
\* recently before it, of those still open; with none there, the tab now at its position, or the last tab if it was
\* the last, or none (0) if no tabs remain: the rule of close
RemoveActiveRule ==
    [][(last'.op = "remove_ai" /\ last'.ok) =>
        LET n == Cardinality(DOMAIN order')
            h == Forget(history, order[last'.i]) IN
        IF active /= last'.i
        THEN order'[active'] = order[active]
        ELSE IF h /= <<>>
        THEN order'[active'] = h[Len(h)]
        ELSE active' = (IF n = 0 THEN 0 ELSE IF last'.i < n THEN last'.i ELSE n - 1)]_vars

=============================================================================
