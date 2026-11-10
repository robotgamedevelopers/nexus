----------------------------- MODULE WMoveSprint -----------------------------
\* Reversed witness: the sprint tab can be dragged: a move from position 0 is
\* taken whatever the zone at `to`, so the sprint tab lands among the friends.
\* SprintNeverMoves must reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs /\ (from = 0 \/ Zone(At(from)) = Zone(At(to)))
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
