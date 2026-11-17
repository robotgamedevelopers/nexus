------------------------- MODULE WDrvAskBlocksStop -------------------------
\* Reversed witness: stopping a running turn interrupts the harness without cancelling its open secret ask, so the tool
\* call stays waiting for the server's secret line (whose dialog outlives a stopped turn). AskOnlyRunning must reject it.
EXTENDS ClaudeDriver

BrokenStopRunning(n) ==
    /\ stopping' = TRUE
    /\ intFor' = n
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, textOpen, failure, pending, carry, inUse, last, secretVars, ghost>>

BrokenStop(n) == StopWith(n, Finish("stopped"), BrokenStopRunning(n))

BrokenSpec == Init /\ [][NextSecretsWith(BrokenStop, Apply, Result, Refuse, NotFound, Ask, Secret)]_vars
=============================================================================
