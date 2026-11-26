-------------------------- MODULE WCloseKeepsHistory --------------------------
\* Reversed witness: close leaves the closed tab's id in the history (and, so
\* that the step is defined, falls back to the rule before the history when the
\* closed tab was active). HistoryOpen must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ active' = ClosedIndex(active, i, Count)
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId, history>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
