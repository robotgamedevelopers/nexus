------------------------- MODULE WAppendDropsQueued -------------------------
\* Reversed witness: append, on a tab with queued prompts, also drops the
\* oldest one. QueuesOnlyGrow must reject it.
EXTENDS MCWindow

BrokenAppend(i, e) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, e)]
         /\ queue' = [queue EXCEPT ![At(i)] = IF Len(@) > 0 THEN Tail(@) ELSE @]
         /\ last' = Did("append", i, "", e, TRUE)
         /\ UNCHANGED <<friends, order, active, draft>>
    ELSE /\ last' = Did("append", i, "", e, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, Queue, BrokenAppend, MoveTab)]_vars
=============================================================================
