------------------------- MODULE WAddAmongTerminals -------------------------
\* Reversed witness: add puts the new terminal before the last tab when that
\* tab is a terminal, so it lands among the terminals, not at the end (the
\* zones stay in order). AddAppends must reject it.
EXTENDS MCWindow

\* where the new tab goes
Slot == IF Count > 0 /\ TabKind(order[Count - 1]) = "Terminal" THEN Count - 1 ELSE Count

BrokenAdd ==
    /\ order' = [p \in 0 .. Count |-> IF p < Slot THEN order[p] ELSE IF p = Slot THEN nextId ELSE order[p - 1]]
    /\ active' = Slot
    /\ nextId' = nextId + 1
    /\ last' = DidAdd(nextId)
    /\ UNCHANGED <<friends, transcript, draft, queue>>

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, BrokenAdd,
                                     CloseTab)]_vars
=============================================================================
