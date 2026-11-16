------------------------- MODULE WLoopReleaseOnFail -------------------------
\* Reversed witness: a failed flush goes on to Drivers::release anyway, delivering commands whose batch
\* was not made durable on disk. DriverOnlyDurable must reject it.
EXTENDS MCLoopPass

BrokenSpec == Init /\ [][NextWith(DefaultOrder, TRUE, "continue", FALSE)]_vars
=============================================================================
