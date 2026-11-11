----------------------------- MODULE WSupStartLoop -----------------------------
\* Reversed witness: failed starts are counted but never stop the supervisor starting drivers, so a driver that cannot
\* start is started again and again. LimitHolds must reject it.
EXTENDS MCSupervisor

NoLimit(x) ==
    LET y == IF x.phase = "ending" THEN x ELSE Abandon(x)
        f == IF ~y.finished /\ ~y.blocked /\ ~y.down THEN y.failed + 1 ELSE y.failed
    IN [y EXCEPT !.phase = "none", !.stage = "none", !.procs = @ - 1, !.failed = f, !.finished = FALSE,
                 !.gathered = FALSE]

BrokenSpec == Init /\ [][NextWith(Prompt, Message, ExitWith(NoLimit), Timeout, Restart)]_vars
=============================================================================
