------------------------ MODULE WSelectClearsDraft ------------------------
\* Reversed witness: select_tab clears the draft of the tab it leaves.
\* SelectExact must reject it.
EXTENDS MCWindow

BrokenSelect(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ draft' = [draft EXCEPT ![active] = ""]
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, queue>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(BrokenSelect, SetDraft, Send, Queue, AppendEntry)]_vars
=============================================================================
