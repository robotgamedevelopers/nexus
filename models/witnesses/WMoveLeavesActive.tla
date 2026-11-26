------------------------- MODULE WMoveLeavesActive -------------------------
\* Reversed witness: move reorders the tabs but leaves active at the old
\* position, so a different AI becomes active. ActiveAIKept must
\* reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs
    THEN /\ order' = Moved(order, from, to)
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, active, transcript, draft, queue>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
