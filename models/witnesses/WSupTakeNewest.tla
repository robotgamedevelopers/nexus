---------------------------- MODULE WSupTakeNewest ----------------------------
\* Reversed witness: at a turn's end the newest queued prompt is taken, not the oldest. TakesAtTurnEnd must reject it.
EXTENDS MCSupervisor

BrokenTake(x) ==
    IF x.open = 0 /\ x.waiting = <<>> /\ ~x.blocked /\ ~x.down /\ x.queue # <<>>
    THEN LET p == x.queue[Len(x.queue)] IN
         [Fx(x, E("P", 0, p)) EXCEPT !.queue = SubSeq(@, 1, Len(@) - 1), !.waiting = <<p>>, !.fate[p] = "waiting"]
    ELSE x

BrokenSpec == Init /\ [][NextTake(BrokenTake)]_vars
=============================================================================
