------------------------- MODULE WQueueToTranscript -------------------------
\* Reversed witness: queue appends the draft to the tab's transcript, as a
\* Person entry, instead of to its queue. QueueLeavesTranscripts must reject it.
EXTENDS MCWindow

BrokenQueue(i) ==
    IF Sendable(i)
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, [kind |-> "Person", text |-> draft[At(i)]])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, queue>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, BrokenQueue, AppendEntry, MoveTab)]_vars
=============================================================================
