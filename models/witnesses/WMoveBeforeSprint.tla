-------------------------- MODULE WMoveBeforeSprint --------------------------
\* Reversed witness: the move rule takes the sprint tab for a friend's (one
\* zone for both), so a friend can be dropped onto position 0 and the sprint
\* tab is pushed behind it. SprintFirst must reject it.
EXTENDS MCWindow

FriendZone(id) == IF TabKind(id) = "Sprint" THEN 1 ELSE Zone(id)

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs /\ FriendZone(At(from)) = FriendZone(At(to))
    THEN /\ order' = Moved(order, from, to)
         /\ active' = MovedIndex(active, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
