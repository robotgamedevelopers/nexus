--------------------------- MODULE WSendToActive ---------------------------
\* Reversed witness: send appends the named tab's draft to the active tab's
\* transcript instead of the named tab's. SendTouchesNoOtherTab must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF Sendable(i)
    THEN /\ transcript' = [transcript EXCEPT ![At(active)] = Append(@, [kind |-> "Person", text |-> draft[At(i)]])]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
