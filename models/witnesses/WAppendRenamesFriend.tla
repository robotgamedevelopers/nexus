------------------------- MODULE WAppendRenamesFriend -------------------------
\* Reversed witness: append also rewrites the friend's entry in the friend list
\* (its model becomes ""). FriendsFixed must reject it.
EXTENDS MCWindow

BrokenAppend(i, e) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, e)]
         /\ friends' = [friends EXCEPT ![At(i) + 1].model = ""]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<order, active, draft, queue>>
    ELSE /\ last' = Did("append", i, "", e, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, BrokenAppend, MoveTab)]_vars
=============================================================================
