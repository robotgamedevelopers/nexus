-------------------------- MODULE WCloseActiveWrong --------------------------
\* Reversed witness: closing the active tab makes the tab before it active,
\* not the one selected before it. CloseActiveRule must reject it.
EXTENDS MCWindow

BrokenIndex(a, i, h) ==
    IF a = i /\ i > 0 THEN i - 1 ELSE ClosedActive(a, i, order, h)

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ history' = Forget(history, At(i))
         /\ active' = BrokenIndex(active, i, Forget(history, At(i)))
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
