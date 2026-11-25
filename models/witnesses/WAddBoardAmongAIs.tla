--------------------------- MODULE WAddBoardAmongAIs ---------------------------
\* Reversed witness: add_board puts the new tab at the end of the AIs' zone (add_ai's place, AIEnd) in place of the
\* dashboards' end, so a dashboard stands behind an AI. ZonesOrdered must reject it.
EXTENDS MCWindow

BrokenAddBoard(name) ==
    IF name /= "" /\ ~BoardStanding(name)
    THEN LET at == AIEnd(order) IN
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
