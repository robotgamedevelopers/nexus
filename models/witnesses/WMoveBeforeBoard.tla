--------------------------- MODULE WMoveBeforeBoard ---------------------------
\* Reversed witness: the move rule takes a dashboard's tab for an AI's (one zone for both), so an AI can be dropped
\* onto a dashboard's position and the dashboard is pushed behind it. ZonesOrdered must reject it.
EXTENDS MCWindow

AIZone(id) == IF TabKind(id) = "Dashboard" THEN 1 ELSE Zone(id)

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs /\ AIZone(At(from)) = AIZone(At(to))
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
