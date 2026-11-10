--------------------------- MODULE WSendAlsoQueues ---------------------------
\* Reversed witness: send appends the draft to the transcript and to the
\* tab's queue as well. OnlyQueueTouchesQueues must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF Sendable(i)
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> "Person", text |-> draft[i]])]
         /\ queue' = [queue EXCEPT ![i] = Append(@, draft[i])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry)]_vars
=============================================================================
