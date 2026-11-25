--------------------- MODULE WClientLockRefusalSilent ---------------------
\* Reversed witness: a refused or cancelled prompt goes back to locked but keeps the old reason, so the lock screen
\* does not say that the attempt failed. RefusalSaysWhy must reject it.
EXTENDS ClientLock

BrokenAnswer(n, a) ==
    /\ n \in pending
    /\ pending' = pending \ {n}
    /\ IF state = "prompting" /\ n = attempt
          THEN IF a = "granted"
                  THEN state' = "unlocked" /\ why' = "none" /\ idle' = 0 /\ granted' = n
                  ELSE state' = "locked" /\ UNCHANGED <<why, idle, granted>>
          ELSE UNCHANGED <<state, why, idle, granted>>
    /\ UNCHANGED attempt
    /\ last' = Rec("answer", a, n, 0, FALSE)

BrokenSpec == Init /\ [][NextWith(Prompt, BrokenAnswer, Relock, Tick, Input, Frame)]_vars
=============================================================================
