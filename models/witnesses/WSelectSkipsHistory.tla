------------------------- MODULE WSelectSkipsHistory -------------------------
\* Reversed witness: select makes its tab active but never records it in the
\* history, so a later close goes back past it. HistoryEndsActive must reject
\* it.
EXTENDS MCWindow

BrokenSelect(i) ==
    IF i \in Tabs
    THEN /\ active' = i
         /\ last' = Did("select", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, transcript, draft, queue, nextId, history>>
    ELSE /\ last' = Did("select", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(BrokenSelect, SetDraft, Send, Queue, AppendEntry, MoveTab)]_vars
=============================================================================
