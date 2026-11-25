----------------------- MODULE WRemoveBoardActiveWrong -----------------------
\* Reversed witness: removing the active dashboard's tab makes the tab now at its position active (the rule before the
\* selection history), not the one selected before it. RemoveActiveRule must reject it.
EXTENDS MCWindow

BrokenRemoveBoard(i) ==
    IF BoardTab(i)
    THEN LET g == At(i) IN
         /\ order' = Removed(order, i)
         /\ history' = ClosedHistory(active, i, order, Forget(history, g))
         /\ active' = ClosedIndex(active, i, Count)
         /\ boards' = Keep(boards, BoardIds \ {g})
         /\ last' = DidBoard("remove_board", i, boards[g], TRUE, g)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidBoard("remove_board", i, "", FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextBoardOps(AddAI, RemoveAI, {}, AddBoard, BrokenRemoveBoard, MCBoardArrivals)]_vars
=============================================================================
