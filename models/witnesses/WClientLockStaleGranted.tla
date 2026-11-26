--------------------- MODULE WClientLockStaleGranted ---------------------
\* Reversed witness: any answer of "granted" unlocks, the answer to an attempt a relock overtook included (the
\* system prompt answered after the Mac slept). UnlockOnlyByCurrent must reject it.
EXTENDS ClientLock

BrokenAnswer(n, a) ==
    /\ n \in pending
    /\ pending' = pending \ {n}
    /\ IF a = "granted"
          THEN state' = "unlocked" /\ why' = "none" /\ idle' = 0 /\ granted' = attempt
          ELSE IF state = "prompting" /\ n = attempt THEN LockFor(a) ELSE UNCHANGED <<state, why, idle, granted>>
    /\ UNCHANGED attempt
    /\ last' = Rec("answer", a, n, 0, FALSE)

BrokenSpec == Init /\ [][NextWith(Prompt, BrokenAnswer, Relock, Tick, Input, Frame)]_vars
=============================================================================
