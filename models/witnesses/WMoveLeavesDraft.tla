------------------------- MODULE WMoveLeavesDraft -------------------------
\* Reversed witness: move carries the AI (its name, transcript and queue)
\* to the new position but leaves the drafts where they stood, so each
\* position keeps the draft it showed and an AI gets another's unsent text.
\* MoveKeepsTabs must reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs
    THEN LET o2 == Moved(order, from, to) IN
         /\ order' = o2
         /\ active' = MovedIndex(active, from, to)
         /\ draft' = [f \in Ids |-> draft[order[CHOOSE p \in Tabs : o2[p] = f]]]
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, transcript, queue>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
