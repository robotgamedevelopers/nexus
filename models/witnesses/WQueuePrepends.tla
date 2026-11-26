--------------------------- MODULE WQueuePrepends ---------------------------
\* Reversed witness: queue puts the draft at the front of the tab's queue,
\* reordering the prompts already there. QueuesOnlyGrow must reject it.
EXTENDS MCWindow

BrokenQueue(i) ==
    IF Sendable(i)
    THEN /\ queue' = [queue EXCEPT ![At(i)] = <<draft[At(i)]>> \o @]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, BrokenQueue, AppendEntry, MoveTab)]_vars
=============================================================================
