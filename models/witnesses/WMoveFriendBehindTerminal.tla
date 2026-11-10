---------------------- MODULE WMoveFriendBehindTerminal ----------------------
\* Reversed witness: a friend's tab moves anywhere, onto a terminal's position
\* among them, so it stands behind a terminal. ZonesOrdered must reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs /\ (TabKind(At(from)) = "Friend" \/ Zone(At(from)) = Zone(At(to)))
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
