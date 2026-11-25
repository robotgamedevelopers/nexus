---------------------------- MODULE WTakeNewest ----------------------------
\* Reversed witness: take_queued takes the newest prompt of the tab's queue, not the oldest, so the server's delivery
\* and the window's queue part ways. TakeExact must reject it.
EXTENDS MCWindow

BrokenTake(i) ==
    IF AITab(i) /\ queue[At(i)] /= <<>>
    THEN /\ queue' = [queue EXCEPT ![At(i)] = SubSeq(@, 1, Len(@) - 1)]
         /\ last' = Did("take", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, transcript, draft, nextId, history>>
    ELSE /\ last' = Did("take", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextTake(BrokenTake)]_vars
=============================================================================
