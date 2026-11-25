----------------------- MODULE WRemoveAIActiveWrong -----------------------
\* Reversed witness: removing the active AI's tab makes the tab before it active, not the one selected before it.
\* RemoveActiveRule must reject it.
EXTENDS MCWindow

BrokenIndex(a, i, h) ==
    IF a = i /\ i > 0 THEN i - 1 ELSE ClosedActive(a, i, order, h)

BrokenRemoveAI(i) ==
    IF AITab(i)
    THEN LET f == At(i) IN
         /\ order' = Removed(order, i)
         /\ history' = ClosedHistory(active, i, order, Forget(history, f))
         /\ active' = BrokenIndex(active, i, Forget(history, f))
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
