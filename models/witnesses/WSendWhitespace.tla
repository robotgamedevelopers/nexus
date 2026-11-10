-------------------------- MODULE WSendWhitespace --------------------------
\* Reversed witness: send and queue accept a draft that is all whitespace
\* (the empty draft among them). SendResult must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> "Person", text |-> draft[i]])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenQueue(i) ==
    IF i \in Tabs
    THEN /\ queue' = [queue EXCEPT ![i] = Append(@, draft[i])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, BrokenQueue, AppendEntry)]_vars
=============================================================================
