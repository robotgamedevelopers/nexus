--------------------------- MODULE WSupTakeWhileOpen ---------------------------
\* Reversed witness: a queued prompt is taken whenever nothing waits, even while a turn is open, so it goes ahead of
\* the turn's end. TakesAtTurnEnd must reject it.
EXTENDS MCSupervisor

BrokenTake(x) ==
    IF x.waiting = <<>> /\ ~x.blocked /\ ~x.down /\ x.queue # <<>>
    THEN LET p == Head(x.queue) IN
         [Fx(x, E("P", 0, p)) EXCEPT !.queue = Tail(@), !.waiting = <<p>>, !.fate[p] = "waiting"]
    ELSE x

BrokenSpec == Init /\ [][NextTake(BrokenTake)]_vars
=============================================================================
