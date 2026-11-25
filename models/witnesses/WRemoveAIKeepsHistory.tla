---------------------- MODULE WRemoveAIKeepsHistory ----------------------
\* Reversed witness: removing an AI's tab leaves its id in the selection history, so the history names a tab that
\* no longer stands (trace_walk's --faulty-remove-ai is this fault in code). HistoryOpen must reject it.
EXTENDS MCWindow

BrokenRemoveAI(i) ==
    IF AITab(i)
    THEN LET f == At(i) IN
         /\ order' = Removed(order, i)
         /\ UNCHANGED history
         /\ active' = ClosedActive(active, i, order, Forget(history, f))
         /\ ais' = Keep(ais, Ids \ {f})
         /\ transcript' = Keep(transcript, Ids \ {f})
         /\ draft' = Keep(draft, Ids \ {f})
         /\ queue' = Keep(queue, Ids \ {f})
         /\ last' = DidAI("remove_ai", i, ais[f], TRUE, f)
         /\ UNCHANGED nextId
    ELSE /\ last' = DidAI("remove_ai", i, NoAI, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextAIOps(AddAI, BrokenRemoveAI, MCArrivals)]_vars
=============================================================================
