----------------------- MODULE WDrvStopDropsSettings -----------------------
\* Reversed witness (found by a cold reader in the driver): a turn stopped before it began drops the settings it was
\* to start with, so the next turn runs without them. SettingsFromNextSend must reject it.
EXTENDS ClaudeDriver

DroppingStop ==
    /\ wrote' = CloseText \o <<W("done-stopped", open)>>
    /\ dones' = [dones EXCEPT ![open] = @ + 1]
    /\ open' = 0
    /\ stage' = "none"
    /\ stopping' = FALSE
    /\ textOpen' = FALSE
    /\ failure' = None
    /\ asking' = 0
    /\ carry' = NoChanges
    /\ UNCHANGED <<phase, resume, intFor, pending, inUse, last, secrets, asks, sent, want, turnWant>>

BrokenStop(n) == StopWith(n, DroppingStop, StopRunning(n))

BrokenSpec == Init /\ [][NextWith(BrokenStop, Apply, Result, Refuse, NotFound)]_vars
=============================================================================
