-------------------------- MODULE WMoveOutOfRange --------------------------
\* Reversed witness: a move to a position past the last tab returns false but
\* moves the tab to the last position anyway. RefusedChangesNothing must
\* reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \notin Tabs /\ N > 0
    THEN /\ order' = Moved(order, from, N - 1)
         /\ active' = MovedIndex(active, from, N - 1)
         /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED <<friends, transcript, draft, queue>>
    ELSE MoveTab(from, to)

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
