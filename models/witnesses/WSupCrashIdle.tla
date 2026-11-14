---------------------------- MODULE WSupCrashIdle ----------------------------
\* Reversed witness: the state cursor's activity forgets that the last driver failed, so a driver that exits mid-turn
\* leaves the friend showing idle, as if waiting for the person. CrashShowsDown must reject it.
EXTENDS MCSupervisor

BrokenActivity(x) ==
    IF x.blocked THEN "down"
    ELSE IF x.open # 0 \/ x.waiting # <<>> \/ x.phase = "starting" THEN "busy"
    ELSE "idle"

BrokenCrashShowsDown == [][CrashShowsDownStep(BrokenActivity)]_vars

BrokenSpec == Init /\ [][Next]_vars
=============================================================================
