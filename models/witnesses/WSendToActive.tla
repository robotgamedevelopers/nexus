--------------------------- MODULE WSendToActive ---------------------------
\* Reversed witness: send appends the named tab's draft to the active tab's
\* transcript instead of the named tab's. SendTouchesNoOtherTab must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF i \in Tabs /\ draft[i] \notin Blank
    THEN /\ transcript' = [transcript EXCEPT ![active] = Append(@, [kind |-> "Person", text |-> draft[i]])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry)]_vars
=============================================================================
