---------------------------- MODULE WCloseSprint ----------------------------
\* Reversed witness: close takes out every tab that is not a friend's, the
\* sprint tab among them, as Command-W on the sprint tab would if the window
\* checked only "not a friend". SprintNeverCloses must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) /= "Friend"
    THEN /\ order' = Removed(order, i)
         /\ active' = ClosedIndex(active, i, Count)
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal,
                                     BrokenClose)]_vars
=============================================================================
