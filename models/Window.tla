------------------------------- MODULE Window -------------------------------
(***************************************************************************)
(* The state the nexus window owns (src/model.h, nexus::Model): one tab   *)
(* per friend, the active tab, and for each tab its transcript, its       *)
(* unsent draft and its queue of prompts held for later. The queue is     *)
(* apart from the transcript: it is the state a server will hold. Tabs    *)
(* are indexed from 0, as in the code.                                    *)
(*                                                                         *)
(* Actions are the operations of nexus::Model. Each takes the arguments   *)
(* the caller passes, including indices out of range and drafts that are  *)
(* all whitespace; a refused operation changes no model state. No         *)
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
    Idx,            \* the indices the operations are tried with, in range and out of it
    Drafts,         \* the draft texts set_draft is tried with
    Blank,          \* the members of Drafts that are all whitespace (the empty string among them)
    AppendEntries   \* the entries append is tried with

ASSUME Blank \subseteq Drafts /\ "" \in Blank

VARIABLES friends, active, transcript, draft, queue, last

model == <<friends, active, transcript, draft, queue>>
vars == <<friends, active, transcript, draft, queue, last>>

N == Len(FriendList)
Tabs == 0 .. (N - 1)
Kinds == {"Person", "Friend", "Notice"}
Texts == Drafts \cup {e.text : e \in AppendEntries}
Entry == [kind : Kinds, text : Texts]
NoEntry == [kind |-> "None", text |-> ""]
Ops == {"init", "select", "set_draft", "send", "queue", "append"}

Did(op, i, t, e, ok) == [op |-> op, i |-> i, text |-> t, entry |-> e, ok |-> ok]

\* send and queue take a draft only on a tab, and only when it is not all whitespace
Sendable(i) == i \in Tabs /\ draft[i] \notin Blank

-----------------------------------------------------------------------------

Init ==
    /\ friends = FriendList
    /\ active = 0
    /\ transcript = [j \in Tabs |-> <<>>]
    /\ draft = [j \in Tabs |-> ""]
    /\ queue = [j \in Tabs |-> <<>>]
    /\ last = Did("init", 0, "", NoEntry, TRUE)

\* select_tab(i)
SelectTab(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* set_draft(i, t)
SetDraft(i, t) ==
    IF i \in Tabs
    THEN /\ draft' = [draft EXCEPT ![i] = t]
         /\ last' = Did("set_draft", i, t, NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, transcript, queue>>
    ELSE /\ last' = Did("set_draft", i, t, NoEntry, FALSE)
         /\ UNCHANGED model

\* send(i): the draft becomes a Person entry at the end of that tab's transcript
Send(i) ==
    IF Sendable(i)
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> "Person", text |-> draft[i]])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* queue(i): the draft goes to the end of that tab's queue
Queue(i) ==
    IF Sendable(i)
    THEN /\ queue' = [queue EXCEPT ![i] = Append(@, draft[i])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* append(i, e)
AppendEntry(i, e) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, e)]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<friends, active, draft, queue>>
    ELSE /\ last' = Did("append", i, "", e, FALSE)
         /\ UNCHANGED model

\* The next-state relation over any set of the five actions. The witnesses
\* (witnesses/*.tla) pass a broken action in place of one of these.
NextWith(Sel(_), SetD(_, _), Snd(_), Que(_), App(_, _)) ==
    \E i \in Idx :
        \/ Sel(i)
        \/ \E t \in Drafts : SetD(i, t)
        \/ Snd(i)
        \/ Que(i)
        \/ \E e \in AppendEntries : App(i, e)

Next == NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* State invariants *)

TypeOK ==
    /\ friends = FriendList
    /\ active \in Int
    /\ transcript \in [Tabs -> Seq(Entry)]
    /\ draft \in [Tabs -> Drafts]
    /\ queue \in [Tabs -> Seq(Drafts \ Blank)]
    /\ last.op \in Ops
    /\ last.i \in Idx \cup {0}
    /\ last.text \in Drafts
    /\ last.entry \in AppendEntries \cup {NoEntry}
    /\ last.ok \in BOOLEAN

\* active is always a tab, or 0 when there are no tabs
ActiveValid == IF N = 0 THEN active = 0 ELSE active \in Tabs

-----------------------------------------------------------------------------
(* Action properties: what each operation does to the state, checked on   *)
(* every step whatever the action that took it.                           *)

Others(i) == Tabs \ {i}

\* select_tab returns true exactly when i is a tab, and changes only active, to i
SelectExact ==
    [][last'.op = "select" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ active' = (IF last'.ok THEN last'.i ELSE active)
        /\ UNCHANGED <<friends, transcript, draft, queue>>]_vars

\* set_draft returns true exactly when i is a tab, and changes only that tab's draft, to the text given
SetDraftExact ==
    [][last'.op = "set_draft" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ draft' = (IF last'.ok THEN [draft EXCEPT ![last'.i] = last'.text] ELSE draft)
        /\ UNCHANGED <<friends, active, transcript, queue>>]_vars

\* send and queue return true exactly when i is a tab whose draft is not all whitespace
SendResult ==
    [][last'.op \in {"send", "queue"} =>
        last'.ok = (last'.i \in Tabs /\ draft[last'.i] \notin Blank)]_vars

\* a send that returns true appends exactly one Person entry, with the draft's text, to that tab's transcript
SendAppendsOne ==
    [][(last'.op = "send" /\ last'.ok) =>
        transcript'[last'.i] = Append(transcript[last'.i], [kind |-> "Person", text |-> draft[last'.i]])]_vars

\* a queue that returns true appends exactly the draft's text to that tab's queue
QueueAppendsOne ==
    [][(last'.op = "queue" /\ last'.ok) =>
        queue'[last'.i] = Append(queue[last'.i], draft[last'.i])]_vars

\* a queue never changes any transcript
QueueLeavesTranscripts ==
    [][last'.op = "queue" => UNCHANGED transcript]_vars

\* only queue changes a queue: send, select, set_draft and append never do
OnlyQueueTouchesQueues ==
    [][last'.op /= "queue" => UNCHANGED queue]_vars

\* a send or queue that returns true clears that tab's draft and no other
SendClearsOnlyItsDraft ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        draft' = [draft EXCEPT ![last'.i] = ""]]_vars

\* a send or queue never touches another tab, the active tab or the friends
SendTouchesNoOtherTab ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        /\ \A j \in Others(last'.i) :
            transcript'[j] = transcript[j] /\ draft'[j] = draft[j] /\ queue'[j] = queue[j]
        /\ UNCHANGED <<friends, active>>]_vars

\* append returns true exactly when i is a tab, and appends exactly that entry to that tab and changes nothing else
AppendExact ==
    [][last'.op = "append" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ transcript' = (IF last'.ok
                          THEN [transcript EXCEPT ![last'.i] = Append(@, last'.entry)]
                          ELSE transcript)
        /\ UNCHANGED <<friends, active, draft, queue>>]_vars

\* an operation that returns false changes no model state
RefusedChangesNothing ==
    [][~last'.ok => UNCHANGED model]_vars

\* s' extends s: its entries are s's, in s's order, then any new ones
Extends(s2, s1) == Len(s2) >= Len(s1) /\ SubSeq(s2, 1, Len(s1)) = s1

\* transcripts only grow by appending: existing entries never change, move or go
TranscriptsOnlyGrow ==
    [][\A j \in Tabs : Extends(transcript'[j], transcript[j])]_vars

\* queues only grow by appending: queued prompts never change, move or go
QueuesOnlyGrow ==
    [][\A j \in Tabs : Extends(queue'[j], queue[j])]_vars

\* the friends a tab shows never change
FriendsFixed == [][UNCHANGED friends]_vars

=============================================================================
