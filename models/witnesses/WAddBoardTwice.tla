---------------------------- MODULE WAddBoardTwice ----------------------------
\* Reversed witness: add_board takes a name a dashboard's tab already has (it checks only for the empty name), so the
\* strip holds two tabs of one dashboard. BoardNamesOnce must reject it.
EXTENDS MCWindow

BrokenAddBoard(name) ==
    IF name /= ""
    THEN LET at == BoardEnd(order) IN
         /\ order' = InsertAt(order, at, nextId)
         /\ active' = IF Count > 0 /\ active >= at THEN active + 1 ELSE active
         /\ boards' = With(boards, nextId, name)
         /\ nextId' = nextId + 1
         /\ history' = IF Count = 0 THEN <<nextId>> ELSE history
         /\ last' = DidBoard("add_board", 0, name, TRUE, nextId)
         /\ UNCHANGED <<ais, transcript, draft, queue>>
    ELSE /\ last' = DidBoard("add_board", 0, name, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextBoardOps(AddAI, RemoveAI, {}, BrokenAddBoard, RemoveBoard, MCBoardArrivals)]_vars
=============================================================================
