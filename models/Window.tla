------------------------------- MODULE Window -------------------------------
(***************************************************************************)
(* The state the nexus window owns (src/model.h, nexus::Model): one tab   *)
(* per friend, the active tab, and for each tab its transcript and its    *)
(* unsent draft. Tabs are indexed from 0, as in the code.                 *)
(*                                                                         *)
(* Actions are the operations of nexus::Model. Each takes the arguments   *)
(* the caller passes, including indices out of range and drafts that are  *)
(* all whitespace; a refused operation changes no model state.            *)
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

VARIABLES friends, active, transcript, draft, last

model == <<friends, active, transcript, draft>>
vars == <<friends, active, transcript, draft, last>>

N == Len(FriendList)
Tabs == 0 .. (N - 1)
Kinds == {"Person", "Queued", "Friend", "Notice"}
Texts == Drafts \cup {e.text : e \in AppendEntries}
Entry == [kind : Kinds, text : Texts]
NoEntry == [kind |-> "None", text |-> ""]
Ops == {"init", "select", "set_draft", "send", "queue", "append"}

Did(op, i, t, e, ok) == [op |-> op, i |-> i, text |-> t, entry |-> e, ok |-> ok]

KindOf(op) == IF op = "send" THEN "Person" ELSE "Queued"

-----------------------------------------------------------------------------

Init ==
    /\ friends = FriendList
    /\ active = 0
    /\ transcript = [j \in Tabs |-> <<>>]
    /\ draft = [j \in Tabs |-> ""]
    /\ last = Did("init", 0, "", NoEntry, TRUE)

\* select_tab(i)
SelectTab(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* set_draft(i, t)
SetDraft(i, t) ==
    IF i \in Tabs
    THEN /\ draft' = [draft EXCEPT ![i] = t]
         /\ last' = Did("set_draft", i, t, NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, transcript>>
    ELSE /\ last' = Did("set_draft", i, t, NoEntry, FALSE)
         /\ UNCHANGED model

\* send(i) and queue(i): op is "send" or "queue"
Submit(op, i) ==
    IF i \in Tabs /\ draft[i] \notin Blank
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> KindOf(op), text |-> draft[i]])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did(op, i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active>>
    ELSE /\ last' = Did(op, i, "", NoEntry, FALSE)
         /\ UNCHANGED model

Send(i) == Submit("send", i)
Queue(i) == Submit("queue", i)

\* append(i, e)
AppendEntry(i, e) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, e)]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<friends, active, draft>>
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
        /\ UNCHANGED <<friends, transcript, draft>>]_vars

\* set_draft returns true exactly when i is a tab, and changes only that tab's draft, to the text given
SetDraftExact ==
    [][last'.op = "set_draft" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ draft' = (IF last'.ok THEN [draft EXCEPT ![last'.i] = last'.text] ELSE draft)
        /\ UNCHANGED <<friends, active, transcript>>]_vars

\* send and queue return true exactly when i is a tab whose draft is not all whitespace
SendResult ==
    [][last'.op \in {"send", "queue"} =>
        last'.ok = (last'.i \in Tabs /\ draft[last'.i] \notin Blank)]_vars

\* a send or queue that returns true appends exactly one entry, of its kind and with the draft's text, to that tab
SendAppendsOne ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        transcript'[last'.i] = Append(transcript[last'.i], [kind |-> KindOf(last'.op), text |-> draft[last'.i]])]_vars

\* a send or queue that returns true clears that tab's draft and no other
SendClearsOnlyItsDraft ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        draft' = [draft EXCEPT ![last'.i] = ""]]_vars

\* a send or queue never touches another tab, the active tab or the friends
SendTouchesNoOtherTab ==
    [][(last'.op \in {"send", "queue"} /\ last'.ok) =>
        /\ \A j \in Others(last'.i) : transcript'[j] = transcript[j] /\ draft'[j] = draft[j]
        /\ UNCHANGED <<friends, active>>]_vars

\* append returns true exactly when i is a tab, and appends exactly that entry to that tab and changes nothing else
AppendExact ==
    [][last'.op = "append" =>
        /\ last'.ok = (last'.i \in Tabs)
        /\ transcript' = (IF last'.ok
                          THEN [transcript EXCEPT ![last'.i] = Append(@, last'.entry)]
                          ELSE transcript)
        /\ UNCHANGED <<friends, active, draft>>]_vars

\* an operation that returns false changes no model state
RefusedChangesNothing ==
    [][~last'.ok => UNCHANGED model]_vars

\* transcripts only grow by appending: existing entries never change, move or go
TranscriptsOnlyGrow ==
    [][\A j \in Tabs :
        /\ Len(transcript'[j]) >= Len(transcript[j])
        /\ SubSeq(transcript'[j], 1, Len(transcript[j])) = transcript[j]]_vars

\* the friends a tab shows never change
FriendsFixed == [][UNCHANGED friends]_vars

=============================================================================
