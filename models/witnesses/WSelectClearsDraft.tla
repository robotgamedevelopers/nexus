------------------------ MODULE WSelectClearsDraft ------------------------
\* Reversed witness: select_tab clears the draft of the tab it leaves.
\* SelectExact must reject it.
EXTENDS MCWindow

BrokenSelect(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ history' = Touch(history, At(i))
         /\ draft' = [draft EXCEPT ![At(active)] = ""]
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, transcript, queue>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(BrokenSelect, SetDraft, Send, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
