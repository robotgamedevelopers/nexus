---------------------- MODULE WMoveAIBehindTerminal ----------------------
\* Reversed witness: an AI's tab moves anywhere, onto a terminal's position
\* among them, so it stands behind a terminal. ZonesOrdered must reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs /\ (TabKind(At(from)) = "AI" \/ Zone(At(from)) = Zone(At(to)))
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
