------------------------- MODULE WLoopResetApplier -------------------------
\* Reversed witness: after a crash between Flush and Tick, a reconnecting window's Applier starts at 0
\* instead of disk.applied[c], applying an already-durable action a second time. ExactlyOnce must reject it.
EXTENDS MCLoopPass

BrokenSpec == Init /\ [][NextWith(DefaultOrder, TRUE, "stop", TRUE)]_vars
=============================================================================
