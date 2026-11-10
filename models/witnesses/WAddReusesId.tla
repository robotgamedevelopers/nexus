---------------------------- MODULE WAddReusesId ----------------------------
\* Reversed witness: add hands out the smallest terminal id that no tab holds
\* now, so a closed terminal's id comes back. AddFreshId must reject it.
EXTENDS MCWindow

Free == {id \in N .. (nextId - 1) : \A p \in Tabs : order[p] /= id}

Reused == IF Free = {} THEN nextId ELSE CHOOSE id \in Free : \A other \in Free : id <= other

BrokenAdd ==
    /\ order' = [p \in 0 .. Count |-> IF p < Count THEN order[p] ELSE Reused]
    /\ active' = Count
    /\ nextId' = IF Free = {} THEN nextId + 1 ELSE nextId
    /\ history' = Touch(history, Reused)
    /\ last' = DidAdd(Reused)
    /\ UNCHANGED <<friends, transcript, draft, queue>>

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, BrokenAdd,
                                     CloseTab)]_vars
=============================================================================
