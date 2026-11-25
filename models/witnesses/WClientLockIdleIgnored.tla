---------------------- MODULE WClientLockIdleIgnored ----------------------
\* Reversed witness: the idle time is counted but never locks (it stops at the timeout). IdleLocks must reject it.
EXTENDS ClientLock

BrokenTick(d) ==
    /\ idle' = IF state = "unlocked" THEN IF idle + d >= Timeout THEN Timeout - 1 ELSE idle + d ELSE idle
    /\ UNCHANGED <<state, why, granted, attempt, pending>>
    /\ last' = Rec("tick", "", 0, d, FALSE)

BrokenSpec == Init /\ [][NextWith(Prompt, Answer, Relock, BrokenTick, Input, Frame)]_vars
=============================================================================
