------------------------- MODULE WCloseActivePastEnd -------------------------
\* Reversed witness: close leaves `active` where it was, so closing the last
\* tab while it is active leaves `active` one past the end. ActiveValid must
\* reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ history' = Forget(history, At(i))
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, active, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
