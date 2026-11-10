---------------------------- MODULE WCloseOldRule ----------------------------
\* Reversed witness: closing the active tab ignores the history and keeps the
\* rule before it, the tab now at its position or the last, so closing the last
\* terminal goes back to the last friend, not the tab selected before it.
\* CloseActiveRule must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ history' = Forget(history, At(i))
         /\ active' = ClosedIndex(active, i, Count)
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
