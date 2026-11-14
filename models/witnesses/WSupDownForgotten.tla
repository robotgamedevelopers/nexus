-------------------------- MODULE WSupDownForgotten --------------------------
\* Reversed witness: the state cursor's activity shows a failed driver as down only while it is being ended, so once
\* it is collected the friend shows idle, with no prompt, no hello and no restart. DownLeavesByWork must reject it.
EXTENDS MCSupervisor

BrokenActivity(x) ==
    IF x.blocked THEN "down"
    ELSE IF x.open # 0 \/ x.waiting # <<>> \/ x.phase = "starting" THEN "busy"
    ELSE IF x.crashed /\ ~x.down /\ x.phase = "ending" THEN "down"
    ELSE "idle"

BrokenDownLeavesByWork == [][DownLeavesByWorkStep(BrokenActivity)]_vars

BrokenSpec == Init /\ [][Next]_vars
=============================================================================
