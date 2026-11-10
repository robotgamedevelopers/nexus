--------------------------- MODULE WQueuePrepends ---------------------------
\* Reversed witness: queue puts the draft at the front of the tab's queue,
\* reordering the prompts already there. QueuesOnlyGrow must reject it.
EXTENDS MCWindow

BrokenQueue(i) ==
    IF Sendable(i)
    THEN /\ queue' = [queue EXCEPT ![i] = <<draft[i]>> \o @]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, BrokenQueue, AppendEntry)]_vars
=============================================================================
