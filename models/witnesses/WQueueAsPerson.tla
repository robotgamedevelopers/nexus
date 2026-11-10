--------------------------- MODULE WQueueAsPerson ---------------------------
\* Reversed witness: queue appends its entry with kind Person instead of
\* Queued. SendAppendsOne must reject it.
EXTENDS MCWindow

BrokenQueue(i) ==
    IF i \in Tabs /\ draft[i] \notin Blank
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> "Person", text |-> draft[i]])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, BrokenQueue, AppendEntry)]_vars
=============================================================================
