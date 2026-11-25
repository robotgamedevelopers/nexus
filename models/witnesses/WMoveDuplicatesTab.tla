------------------------- MODULE WMoveDuplicatesTab -------------------------
\* Reversed witness: move puts the tab in at `to` without taking it out at
\* `from`, so that AI has two tabs. NoTabDuplicated must reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs
    THEN /\ order' = [p \in 0 .. N |-> IF p < to THEN order[p] ELSE IF p = to THEN order[from] ELSE order[p - 1]]
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<ais, active, transcript, draft, queue>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
