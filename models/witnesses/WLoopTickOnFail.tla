-------------------------- MODULE WLoopTickOnFail --------------------------
\* Reversed witness: after a failed flush the loop continues to the next pass's Tick instead of stopping,
\* acknowledging actions that never became durable. AckedOnlyDurable must reject it.
EXTENDS MCLoopPass

BrokenSpec == Init /\ [][NextWith(DefaultOrder, TRUE, "loop", FALSE)]_vars
=============================================================================
