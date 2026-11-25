----------------------- MODULE WRemoveBoardKeepsHistory -----------------------
\* Reversed witness: removing a dashboard's tab leaves its id in the selection history, as if the history were the
\* strip's alone to keep (trace_walk's --faulty-remove-board is this fault in code). RemoveForgets must reject it.
EXTENDS MCWindow

BrokenRemoveBoard(i) ==
    IF BoardTab(i)
    THEN LET g == At(i) IN
         /\ order' = Removed(order, i)
         /\ UNCHANGED history
         /\ active' = ClosedActive(active, i, order, Forget(history, g))
         /\ boards' = Keep(boards, BoardIds \ {g})
         /\ last' = DidBoard("remove_board", i, boards[g], TRUE, g)
         /\ UNCHANGED <<ais, transcript, draft, queue, nextId>>
    ELSE /\ last' = DidBoard("remove_board", i, "", FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextBoardOps(AddAI, RemoveAI, {}, AddBoard, BrokenRemoveBoard, MCBoardArrivals)]_vars
=============================================================================
