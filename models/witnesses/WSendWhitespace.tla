-------------------------- MODULE WSendWhitespace --------------------------
\* Reversed witness: send and queue accept a draft that is all whitespace
\* (the empty draft among them). SendResult must reject it.
EXTENDS MCWindow

BrokenSubmit(op, i) ==
    IF i \in Tabs
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> KindOf(op), text |-> draft[i]])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did(op, i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active>>
    ELSE /\ last' = Did(op, i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSend(i) == BrokenSubmit("send", i)
BrokenQueue(i) == BrokenSubmit("queue", i)

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, BrokenQueue, AppendEntry)]_vars
=============================================================================
