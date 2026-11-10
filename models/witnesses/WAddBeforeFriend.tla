-------------------------- MODULE WAddBeforeFriend --------------------------
\* Reversed witness: add puts the new terminal first, before the friends.
\* ZonesOrdered must reject it.
EXTENDS MCWindow

BrokenAdd ==
    /\ order' = [p \in 0 .. Count |-> IF p = 0 THEN nextId ELSE order[p - 1]]
    /\ active' = 0
    /\ nextId' = nextId + 1
    /\ history' = Touch(history, nextId)
    /\ last' = DidAdd(nextId)
    /\ UNCHANGED <<friends, transcript, draft, queue>>

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, BrokenAdd,
                                     CloseTab)]_vars
=============================================================================
