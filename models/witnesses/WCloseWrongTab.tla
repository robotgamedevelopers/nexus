--------------------------- MODULE WCloseWrongTab ---------------------------
\* Reversed witness: close of a terminal's tab takes out the last tab instead,
\* so closing the first of two terminals closes the second. CloseRemovesExactly
\* must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, Count - 1)
         /\ active' = ClosedIndex(active, Count - 1, Count)
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
