------------------------------- MODULE Window -------------------------------
(***************************************************************************)
(* The state the nexus window owns (src/model.h, nexus::Model): one tab   *)
(* per friend, the order the tabs stand in, the active tab, and for each  *)
(* friend its transcript, its unsent draft and its queue of prompts held  *)
(* for later. The queue is apart from the transcript: it is the state a   *)
(* server will hold.                                                       *)
(*                                                                         *)
(* A friend is named by its place in the constructor's list, its id, from *)
(* 0; that never changes. A tab's position is its place in the strip,     *)
(* from 0, as in the code; move_tab changes it. `order` maps each position *)
(* to the friend whose tab stands there, and the per-friend state is      *)
(* indexed by friend, so it moves with the tab. `active` is a position,   *)
(* as in the code, and a move carries it with the same arithmetic.        *)
(*                                                                         *)
(* Actions are the operations of nexus::Model. Each takes the arguments   *)
(* the caller passes, positions in range and out of it and drafts that    *)
(* are all whitespace; a refused operation changes no model state. No     *)
(* operation takes from a queue: delivery belongs to a later layer.       *)
(*                                                                         *)
(* `last` is not model state. It is a history variable: the operation     *)
(* just performed, its arguments and the value it returned. It lets the   *)
(* per-operation guarantees below be stated as action properties, and it  *)
(* is what a code trace's return values are bound to (WindowTrace.tla).   *)
(***************************************************************************)
EXTENDS Integers, Sequences

CONSTANTS
    FriendList,     \* <<[name |-> .., model |-> ..], ...>>, the constructor's argument
    Idx,            \* the positions the operations are tried with, in range and out of it
    Drafts,         \* the draft texts set_draft is tried with
    Blank,          \* the members of Drafts that are all whitespace (the empty string among them)
    AppendEntries   \* the entries append is tried with

ASSUME Blank \subseteq Drafts /\ "" \in Blank

VARIABLES friends, order, active, transcript, draft, queue, last

model == <<friends, order, active, transcript, draft, queue>>
vars == <<friends, order, active, transcript, draft, queue, last>>

N == Len(FriendList)
Tabs == 0 .. (N - 1)        \* positions in the strip
Ids == 0 .. (N - 1)         \* friends, by their place in FriendList
Kinds == {"Person", "Friend", "Notice"}
Texts == Drafts \cup {e.text : e \in AppendEntries}
Entry == [kind : Kinds, text : Texts]
NoEntry == [kind |-> "None", text |-> ""]
Ops == {"init", "select", "set_draft", "send", "queue", "append", "move"}

Did(op, i, t, e, ok) == [op |-> op, i |-> i, j |-> 0, text |-> t, entry |-> e, ok |-> ok]
DidMove(from, to, ok) == [op |-> "move", i |-> from, j |-> to, text |-> "", entry |-> NoEntry, ok |-> ok]

\* the friend whose tab stands at position i
At(i) == order[i]

\* send and queue take a draft only on a tab, and only when it is not all whitespace
Sendable(i) == i \in Tabs /\ draft[At(i)] \notin Blank

\* The order after the tab at `from` is taken out and put back in at `to`: the tabs
\* between shift by one toward the place it left. The code's std::rotate.
Moved(o, from, to) ==
    [p \in Tabs |->
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

-----------------------------------------------------------------------------

Init ==
    /\ friends = FriendList
    /\ order = [p \in Tabs |-> p]
    /\ active = 0
    /\ transcript = [f \in Ids |-> <<>>]
    /\ draft = [f \in Ids |-> ""]
    /\ queue = [f \in Ids |-> <<>>]
    /\ last = Did("init", 0, "", NoEntry, TRUE)

\* select_tab(i)
SelectTab(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, transcript, draft, queue>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* set_draft(i, t)
SetDraft(i, t) ==
    IF i \in Tabs
    THEN /\ draft' = [draft EXCEPT ![At(i)] = t]
         /\ last' = Did("set_draft", i, t, NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, active, transcript, queue>>
    ELSE /\ last' = Did("set_draft", i, t, NoEntry, FALSE)
         /\ UNCHANGED model

\* send(i): the draft becomes a Person entry at the end of that tab's transcript
Send(i) ==
    IF Sendable(i)
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, [kind |-> "Person", text |-> draft[At(i)]])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* queue(i): the draft goes to the end of that tab's queue
Queue(i) ==
    IF Sendable(i)
    THEN /\ queue' = [queue EXCEPT ![At(i)] = Append(@, draft[At(i)])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* append(i, e)
AppendEntry(i, e) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, e)]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<friends, order, active, draft, queue>>
    ELSE /\ last' = Did("append", i, "", e, FALSE)
         /\ UNCHANGED model

\* move_tab(from, to): the tab at `from` ends at `to`, and active follows its friend
MoveTab(from, to) ==
    IF from \in Tabs /\ to \in Tabs
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

\* The next-state relation over any set of the six actions. The witnesses
\* (witnesses/*.tla) pass a broken action in place of one of these.
NextWith(Sel(_), SetD(_, _), Snd(_), Que(_), App(_, _), Mov(_, _)) ==
    \E i \in Idx :
        \/ Sel(i)
        \/ \E t \in Drafts : SetD(i, t)
        \/ Snd(i)
        \/ Que(i)
        \/ \E e \in AppendEntries : App(i, e)
        \/ \E j \in Idx : Mov(i, j)

Next == NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* State invariants *)

TypeOK ==
    /\ friends = FriendList
    /\ order \in [Tabs -> Ids]
    /\ active \in Int
    /\ transcript \in [Ids -> Seq(Entry)]
    /\ draft \in [Ids -> Drafts]
    /\ queue \in [Ids -> Seq(Drafts \ Blank)]
    /\ last.op \in Ops
    /\ last.i \in Idx \cup {0}
    /\ last.j \in Idx \cup {0}
    /\ last.text \in Drafts
    /\ last.entry \in AppendEntries \cup {NoEntry}
    /\ last.ok \in BOOLEAN

\* active is always a tab, or 0 when there are no tabs
ActiveValid == IF N = 0 THEN active = 0 ELSE active \in Tabs

\* every friend has a tab: no reordering loses one
NoTabLost == \A f \in Ids : \E p \in DOMAIN order : order[p] = f

\* no friend has two tabs: no reordering duplicates one
NoTabDuplicated == \A p, q \in DOMAIN order : p /= q => order[p] /= order[q]

-----------------------------------------------------------------------------
(* Action properties: what each operation does to the state, checked on   *)
(* every step whatever the action that took it. Each is stated per        *)
(* friend: the operation on position i acts on At(i), the friend there.   *)

Others(f) == Ids \ {f}

\* select_tab returns true exactly when i is a tab, and changes only active, to i
SelectExact ==
    [][last'.op = "select" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ active' = (IF last'.ok THEN last'.i ELSE active)
        /\ UNCHANGED <<friends, order, transcript, draft, queue>>]_vars

\* set_draft returns true exactly when i is a tab, and changes only that friend's draft, to the text given
SetDraftExact ==
    [][last'.op = "set_draft" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ draft' = (IF last'.ok THEN [draft EXCEPT ![At(last'.i)] = last'.text] ELSE draft)
        /\ UNCHANGED <<friends, order, active, transcript, queue>>]_vars

\* send and queue return true exactly when i is a tab whose friend's draft is not all whitespace
SendResult ==
    [][last'.op \in {"send", "queue"} =>
        last'.ok = (last'.i \in Tabs /\ draft[At(last'.i)] \notin Blank)]_vars

\* a send that returns true appends exactly one Person entry, with the draft's text, to that friend's transcript
SendAppendsOne ==
    [][(last'.op = "send" /\ last'.ok) =>
        LET f == At(last'.i) IN
        transcript'[f] = Append(transcript[f], [kind |-> "Person", text |-> draft[f]])]_vars

\* a queue that returns true appends exactly the draft's text to that friend's queue
QueueAppendsOne ==
    [][(last'.op = "queue" /\ last'.ok) =>
        LET f == At(last'.i) IN queue'[f] = Append(queue[f], draft[f])]_vars

\* a queue never changes any transcript
QueueLeavesTranscripts ==
    [][last'.op = "queue" => UNCHANGED transcript]_vars

\* only queue changes a queue: send, select, set_draft, append and move never do
OnlyQueueTouchesQueues ==
    [][last'.op /= "queue" => UNCHANGED queue]_vars

\* a send or queue that returns true clears that friend's draft and no other
SendClearsOnlyItsDraft ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        draft' = [draft EXCEPT ![At(last'.i)] = ""]]_vars

\* a send or queue never touches another friend, the order, the active tab or the friends
SendTouchesNoOtherTab ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        /\ \A g \in Others(At(last'.i)) :
            transcript'[g] = transcript[g] /\ draft'[g] = draft[g] /\ queue'[g] = queue[g]
        /\ UNCHANGED <<friends, order, active>>]_vars

\* append returns true exactly when i is a tab, appends exactly that entry to that friend and changes nothing else
AppendExact ==
    [][last'.op = "append" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ transcript' = (IF last'.ok
                          THEN [transcript EXCEPT ![At(last'.i)] = Append(@, last'.entry)]
                          ELSE transcript)
        /\ UNCHANGED <<friends, order, active, draft, queue>>]_vars

\* the order as a sequence of friends, from the first position, leaving out friend f
Without(o, f) == SelectSeq([k \in 1 .. Len(FriendList) |-> o[k - 1]], LAMBDA g : g /= f)

\* move returns true exactly when both positions are tabs; then the friend that stood at `from`
\* stands at `to`, and every other friend keeps its place relative to the rest
MovePlaces ==
    [][last'.op = "move" =>
        /\ last'.ok = (last'.i \in Tabs /\ last'.j \in Tabs)
        /\ last'.ok =>
            /\ order'[last'.j] = order[last'.i]
            /\ Without(order', order[last'.i]) = Without(order, order[last'.i])]_vars

\* a move changes no friend's transcript, draft or queue, nor the friends: they go with the tab
MoveKeepsTabs ==
    [][last'.op = "move" => UNCHANGED <<friends, transcript, draft, queue>>]_vars

\* only a move reorders the tabs
OnlyMoveReorders ==
    [][last'.op /= "move" => UNCHANGED order]_vars

\* every operation but select keeps the same friend active, wherever its tab now stands
ActiveFriendKept ==
    [][(N > 0 /\ last'.op /= "select") => order'[active'] = order[active]]_vars

\* an operation that returns false changes no model state
RefusedChangesNothing ==
    [][~last'.ok => UNCHANGED model]_vars

\* s' extends s: its entries are s's, in s's order, then any new ones
Extends(s2, s1) == Len(s2) >= Len(s1) /\ SubSeq(s2, 1, Len(s1)) = s1

\* transcripts only grow by appending: existing entries never change, move or go
TranscriptsOnlyGrow ==
    [][\A f \in Ids : Extends(transcript'[f], transcript[f])]_vars

\* queues only grow by appending: queued prompts never change, move or go
QueuesOnlyGrow ==
    [][\A f \in Ids : Extends(queue'[f], queue[f])]_vars

\* the friends never change
FriendsFixed == [][UNCHANGED friends]_vars

=============================================================================
