--------------------------- MODULE WMoveDropsTab ---------------------------
\* Reversed witness: move takes the tab out at `from` and never puts it back,
\* so the strip is one tab short and that friend has none. NoTabLost must
\* reject it.
EXTENDS MCWindow

BrokenMove(from, to) ==
    IF from \in Tabs /\ to \in Tabs
    THEN /\ order' = [p \in 0 .. (N - 2) |-> IF p < from THEN order[p] ELSE order[p + 1]]
         /\ last' = DidMove(from, to, TRUE)
         /\ UNCHANGED <<friends, active, transcript, draft, queue>>
    ELSE /\ last' = DidMove(from, to, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, BrokenMove)]_vars
=============================================================================
