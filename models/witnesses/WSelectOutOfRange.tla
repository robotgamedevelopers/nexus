------------------------- MODULE WSelectOutOfRange -------------------------
\* Reversed witness: select_tab with an index out of range returns false but
\* moves active there anyway. ActiveValid must reject it.
EXTENDS MCWindow

BrokenSelect(i) ==
    /\ active' = i
    /\ last' = Did("select", i, "", NoEntry, i \in Tabs)
    /\ UNCHANGED <<friends, transcript, draft>>

BrokenSpec == Init /\ [][NextWith(BrokenSelect, SetDraft, Send, Queue, AppendEntry)]_vars
=============================================================================
