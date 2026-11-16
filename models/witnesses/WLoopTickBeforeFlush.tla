------------------------ MODULE WLoopTickBeforeFlush ------------------------
\* Reversed witness: links tick after Apply and before DurableStore::flush, so mosh acknowledges
\* received client actions before they are durable on disk. AckedOnlyDurable must reject it.
EXTENDS MCLoopPass

BrokenOrder == <<"hold", "apply", "tick", "flush", "release", "state">>
BrokenSpec == Init /\ [][NextWith(BrokenOrder, TRUE, "stop", FALSE)]_vars
=============================================================================
