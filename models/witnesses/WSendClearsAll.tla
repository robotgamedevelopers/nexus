--------------------------- MODULE WSendClearsAll ---------------------------
\* Reversed witness: a send that succeeds clears every tab's draft, not only
\* its own. SendClearsOnlyItsDraft must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF Sendable(i)
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> "Person", text |-> draft[i]])]
         /\ draft' = [j \in Tabs |-> ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry)]_vars
=============================================================================
