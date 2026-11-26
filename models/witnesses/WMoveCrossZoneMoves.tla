------------------------- MODULE WMoveCrossZoneMoves -------------------------
\* Reversed witness: a move across zones returns false, as it should, but
\* moves the tab anyway. RefusedChangesNothing must reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, Zone(At(from)) = Zone(At(to)))
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
