---------------------- MODULE WMoveTerminalBeforeAI ----------------------
\* Reversed witness: a terminal's tab moves anywhere, onto an AI's position
\* among them, so it stands before an AI. ZonesOrdered must reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs /\ (TabKind(At(from)) = "Terminal" \/ Zone(At(from)) = Zone(At(to)))
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
