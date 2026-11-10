------------------------ MODULE WRefusedClearsDraft ------------------------
\* Reversed witness: a send refused for an all-whitespace draft returns false
\* but clears the draft. RefusedChangesNothing must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF i \in Tabs /\ draft[i] \notin Blank
    THEN Send(i)
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ draft' = (IF i \in Tabs THEN [draft EXCEPT ![i] = ""] ELSE draft)
         /\ UNCHANGED <<friends, active, transcript, queue>>

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry)]_vars
=============================================================================
