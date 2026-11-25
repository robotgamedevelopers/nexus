-------------------- MODULE WClientLockDrawsPrompting --------------------
\* Reversed witness: a frame draws the content as soon as the system prompt is up, before its answer. ContentOnlyUnlocked
\* must reject it.
EXTENDS ClientLock

BrokenFrame ==
    /\ UNCHANGED <<state, why, attempt, pending, idle, granted>>
    /\ last' = Rec("frame", "", 0, 0, state # "locked")

BrokenSpec == Init /\ [][NextWith(Prompt, Answer, Relock, Tick, Input, BrokenFrame)]_vars
=============================================================================
