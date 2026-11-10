---------------------------- MODULE WSelectReorders ----------------------------
\* Reversed witness: select brings the selected tab to the front of the order
\* (and makes position 0 active, so the same friend is selected). Every friend
\* keeps one tab; OnlyMoveReorders must reject it.
EXTENDS MCWindow

BrokenSelect(i) ==
    IF i \in Tabs
    THEN /\ order' = Moved(order, i, 0)
         /\ active' = 0
         /\ history' = Touch(history, At(i))
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(BrokenSelect, SetDraft, Send, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
