-------------------------- MODULE WSupIdleWhileWaiting --------------------------
\* Reversed witness: the state cursor's activity forgets the prompts that wait, so a prompt sent while a driver is
\* being ended for an account change shows the AI idle, as if waiting for the person. IdleWaitsForPerson must
\* reject it.
EXTENDS MCSupervisor

BrokenActivity(x) ==
    IF x.blocked THEN "down"
    ELSE IF x.open # 0 \/ x.phase = "starting" THEN "busy"
    ELSE IF x.crashed /\ ~x.down THEN "down"
    ELSE "idle"

BrokenIdleWaitsForPerson == IdleWaitsForPersonWith(BrokenActivity)

BrokenSpec == Init /\ [][Next]_vars
=============================================================================
