------------------- MODULE WClientLockInputWhileLocked -------------------
\* Reversed witness: input reaches nexus whatever the state, as a hotkey or a key typed under the lock screen would.
\* InputOnlyUnlocked must reject it.
EXTENDS ClientLock

BrokenInput ==
    /\ idle' = IF state = "unlocked" THEN 0 ELSE idle
    /\ UNCHANGED <<state, why, attempt, pending, granted>>
    /\ last' = Rec("input", "", 0, 0, TRUE)

BrokenSpec == Init /\ [][NextWith(Prompt, Answer, Relock, Tick, BrokenInput, Frame)]_vars
=============================================================================
