--------------------------- MODULE WSendClearsAll ---------------------------
\* Reversed witness: a send that succeeds clears every AI's draft, not only
\* its own. SendClearsOnlyItsDraft must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF Sendable(i)
    THEN /\ transcript' = [transcript EXCEPT ![At(i)] = Append(@, [kind |-> "Person", text |-> draft[At(i)]])]
         /\ draft' = [f \in Ids |-> ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
