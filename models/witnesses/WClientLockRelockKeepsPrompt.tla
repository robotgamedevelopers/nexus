------------------- MODULE WClientLockRelockKeepsPrompt -------------------
\* Reversed witness: a relock while the system prompt is up leaves it prompting, so an answer after the Mac slept
\* still unlocks. RelockLocks must reject it.
EXTENDS ClientLock

BrokenRelock(r) ==
    /\ IF state = "prompting" THEN UNCHANGED <<state, why, idle, granted>> ELSE LockFor(r)
    /\ UNCHANGED <<attempt, pending>>
    /\ last' = Rec("relock", r, 0, 0, FALSE)

BrokenSpec == Init /\ [][NextWith(Prompt, Answer, BrokenRelock, Tick, Input, Frame)]_vars
=============================================================================
