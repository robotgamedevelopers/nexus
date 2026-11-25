-------------------- MODULE WClientLockStartsUnlocked --------------------
\* Reversed witness: the window starts unlocked, as one that skipped the prompt at launch would. UnlockedByAuth
\* must reject it.
EXTENDS ClientLock

BrokenInit ==
    /\ state = "unlocked"
    /\ why = "none"
    /\ attempt = 0
    /\ pending = {}
    /\ idle = 0
    /\ granted = 0
    /\ last = Rec("init", "", 0, 0, FALSE)

BrokenSpec == BrokenInit /\ [][Next]_vars
=============================================================================
