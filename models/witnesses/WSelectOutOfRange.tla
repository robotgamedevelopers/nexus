------------------------- MODULE WSelectOutOfRange -------------------------
\* Reversed witness: select_tab with an index out of range returns false but
\* moves active there anyway. ActiveValid must reject it.
EXTENDS MCWindow

BrokenSelect(i) ==
    /\ active' = i
    /\ history' = IF i \in Tabs THEN Touch(history, At(i)) ELSE history
    /\ last' = Did("select", i, "", NoEntry, i \in Tabs)
    /\ UNCHANGED <<friends, order, transcript, draft, queue>>

BrokenSpec == Init /\ [][NextWith(BrokenSelect, SetDraft, Send, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
