---------------------------- MODULE WSupNoDriverIdle ----------------------------
\* Reversed witness: the state cursor's activity forgets that the friend has no driver configured, so a friend whose
\* driver the drivers file lost shows idle (dark blue), as if waiting for the person, though a prompt to it reaches no
\* driver. NoDriverShowsDown must reject it.
EXTENDS MCSupervisor

BrokenActivity(x) ==
    IF x.blocked THEN "down"
    ELSE IF x.open # 0 \/ x.waiting # <<>> \/ x.phase = "starting" THEN "busy"
    ELSE IF x.crashed /\ ~x.down THEN "down"
    ELSE "idle"

BrokenNoDriverShowsDown == NoDriverShowsDownWith(BrokenActivity)

BrokenSpec == Init /\ [][Next]_vars
=============================================================================
