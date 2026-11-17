------------------------- MODULE WSupTakeWhileAbsent -------------------------
\* Reversed witness: the take forgets the friend is out of friends.json, so a prompt queued while it is out leaves the
\* queue and waits, for a friend that is gone. AbsentKeepsPrompts must reject it.
EXTENDS MCSupervisor

BrokenTake(x) ==
    IF ~x.unconf /\ x.here /\ x.open = 0 /\ x.waiting = <<>> /\ ~x.blocked /\ ~x.down /\ x.queue # <<>>
    THEN LET p == Head(x.queue) IN
         [Fx(x, E("P", 0, p)) EXCEPT !.queue = Tail(@), !.waiting = <<p>>, !.fate[p] = "waiting"]
    ELSE x

BrokenSpec == Init /\ [][NextTake(BrokenTake)]_vars
=============================================================================
