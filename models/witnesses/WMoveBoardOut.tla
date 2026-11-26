---------------------------- MODULE WMoveBoardOut ----------------------------
\* Reversed witness: a dashboard can be dragged anywhere: a move from a dashboard's tab is taken whatever the zone at
\* `to`, so a dashboard lands among the AIs or the terminals. MovePlaces must reject it: a move across zones returned
\* true.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs /\ (Zone(At(from)) = 0 \/ Zone(At(from)) = Zone(At(to)))
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
