-------------------------- MODULE WQueueAppendsTwice --------------------------
\* Reversed witness: queue appends the draft twice. Queues still only grow and no
\* transcript changes; QueueAppendsOne must reject it.
EXTENDS MCWindow

BrokenQueue(i) ==
    IF Sendable(i)
    THEN /\ queue' = [queue EXCEPT ![At(i)] = Append(Append(@, draft[At(i)]), draft[At(i)])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, BrokenQueue, AppendEntry, MoveTab)]_vars
=============================================================================
