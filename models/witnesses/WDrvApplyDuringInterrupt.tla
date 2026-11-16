---------------------- MODULE WDrvApplyDuringInterrupt ----------------------
\* Reversed witness: the next turn starts without waiting for the last turn's interrupt to answer, so the interrupt
\* can land in it. StopNeverLater must reject it.
EXTENDS ClaudeDriver

BrokenApply ==
    /\ phase = "ready"
    /\ stage = "waiting"
    /\ ~stopping
    /\ stage' = "applying"
    /\ inUse' = Over(inUse, carry)
    /\ carry' = NoChanges
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stopping, textOpen, failure, intFor, pending, last, secretVars, ghost>>

BrokenSpec == Init /\ [][NextWith(Stop, BrokenApply, Result, Refuse, NotFound)]_vars
=============================================================================
