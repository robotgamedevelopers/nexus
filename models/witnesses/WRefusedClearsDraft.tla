------------------------ MODULE WRefusedClearsDraft ------------------------
\* Reversed witness: a send refused for an all-whitespace draft returns false
\* but clears the draft. RefusedChangesNothing must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF Sendable(i)
    THEN Send(i)
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ draft' = (IF i \in Tabs THEN [draft EXCEPT ![At(i)] = ""] ELSE draft)
         /\ UNCHANGED <<ais, order, active, transcript, queue>>

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
