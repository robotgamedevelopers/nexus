---------------------------- MODULE WSupTakeKeeps ----------------------------
\* Reversed witness: a queued prompt taken at a turn's end joins the waiting prompts but stays in the queue too, so it
\* would be taken again at the next turn's end. QueueSound must reject it.
EXTENDS MCSupervisor

BrokenTake(x) ==
    IF x.open = 0 /\ x.waiting = <<>> /\ ~x.blocked /\ ~x.down /\ x.queue # <<>>
    THEN LET p == Head(x.queue) IN
         [Fx(x, E("P", 0, p)) EXCEPT !.waiting = <<p>>, !.fate[p] = "waiting"]
    ELSE x

BrokenSpec == Init /\ [][NextTake(BrokenTake)]_vars
=============================================================================
