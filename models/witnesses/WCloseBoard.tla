----------------------------- MODULE WCloseBoard -----------------------------
\* Reversed witness: close takes out every tab that is not an AI's, a dashboard's among them, as Command-W on a
\* dashboard would if the window checked only "not an AI". BoardNeverCloses must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) /= "AI"
    THEN /\ order' = Removed(order, i)
         /\ history' = ClosedHistory(active, i, order, Forget(history, At(i)))
         /\ active' = ClosedActive(active, i, order, Forget(history, At(i)))
         /\ boards' = Keep(boards, BoardIds \ {At(i)})
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

\* BrokenClose changes the dashboards, so it goes beside NextWithAll's other actions, not through its Cls (which keeps
\* them).
BrokenSpec == Init /\ [][\/ NextWith(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab)
                         \/ \E i \in Idx : BrokenClose(i)]_vars
=============================================================================
