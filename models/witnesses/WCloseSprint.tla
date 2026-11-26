---------------------------- MODULE WCloseSprint ----------------------------
\* Reversed witness: close takes out every tab that is not an AI's, the
\* sprint tab among them, as Command-W on the sprint tab would if the window
\* checked only "not an AI". SprintNeverCloses must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) /= "AI"
    THEN /\ order' = Removed(order, i)
         /\ history' = Forget(history, At(i))
         /\ active' = ClosedActive(active, i, order, Forget(history, At(i)))
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
