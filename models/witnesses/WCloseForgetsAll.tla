--------------------------- MODULE WCloseForgetsAll ---------------------------
\* Reversed witness: close empties the whole history, not only the closed
\* tab's id, so every close of the active tab falls back to the rule before
\* the history. CloseForgets must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ history' = <<>>
         /\ active' = ClosedActive(active, i, order, <<>>)
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
