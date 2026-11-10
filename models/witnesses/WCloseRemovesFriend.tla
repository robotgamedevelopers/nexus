------------------------ MODULE WCloseRemovesFriend ------------------------
\* Reversed witness: close takes out any tab, a friend's too, so that friend
\* has no tab. NoTabLost must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs
    THEN /\ order' = Removed(order, i)
         /\ history' = Forget(history, At(i))
         /\ active' = ClosedActive(active, i, order, Forget(history, At(i)))
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
