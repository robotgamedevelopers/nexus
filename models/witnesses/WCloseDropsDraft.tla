-------------------------- MODULE WCloseDropsDraft --------------------------
\* Reversed witness: closing a terminal's tab also clears every friend's
\* draft. CloseKeepsFriends must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ history' = Forget(history, At(i))
         /\ active' = ClosedActive(active, i, order, Forget(history, At(i)))
         /\ draft' = [f \in Ids |-> ""]
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
