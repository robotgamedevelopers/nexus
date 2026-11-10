-------------------------- MODULE WCloseActiveWrong --------------------------
\* Reversed witness: closing the active tab makes the tab before it active,
\* not the one that takes its position. CloseActiveRule must reject it.
EXTENDS MCWindow

BrokenIndex(a, i, n) ==
    IF a = i /\ i > 0 THEN i - 1 ELSE ClosedIndex(a, i, n)

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ active' = BrokenIndex(active, i, Count)
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
