--------------------------- MODULE WSendClearsAll ---------------------------
\* Reversed witness: a send or queue that succeeds clears every tab's draft,
\* not only its own. SendClearsOnlyItsDraft must reject it.
EXTENDS MCWindow

BrokenSubmit(op, i) ==
    IF i \in Tabs /\ draft[i] \notin Blank
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> KindOf(op), text |-> draft[i]])]
         /\ draft' = [j \in Tabs |-> ""]
         /\ last' = Did(op, i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active>>
    ELSE /\ last' = Did(op, i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSend(i) == BrokenSubmit("send", i)
BrokenQueue(i) == BrokenSubmit("queue", i)

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, BrokenQueue, AppendEntry)]_vars
=============================================================================
