------------------------- MODULE WMoveLeavesActive -------------------------
\* Reversed witness: move reorders the tabs but leaves active at the old
\* position, so a different friend becomes active. ActiveFriendKept must
\* reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs
    THEN /\ order' = Moved(order, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<friends, active, transcript, draft, queue>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
