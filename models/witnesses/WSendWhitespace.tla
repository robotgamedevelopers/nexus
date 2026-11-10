-------------------------- MODULE WSendWhitespace --------------------------
\* Reversed witness: send and queue accept a draft that is all whitespace
\* (the empty draft among them). SendResult must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, [kind |-> "Person", text |-> draft[At(i)]])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenQueue(i) ==
    IF i \in Tabs
    THEN /\ queue' = [queue EXCEPT ![At(i)] = Append(@, draft[At(i)])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, BrokenQueue, AppendEntry, MoveTab)]_vars
=============================================================================
