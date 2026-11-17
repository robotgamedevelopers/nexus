------------------------ MODULE WLoopStateBeforeFlush ------------------------
\* Reversed witness: ServerSession::setState runs before DurableStore::flush, staging an uncommitted
\* batch's state on the links ahead of what is durable on disk. LinkStateDurable must reject it.
EXTENDS MCLoopPass

BrokenOrder == <<"tick", "hold", "apply", "state", "flush", "release">>
BrokenSpec == Init /\ [][NextWith(BrokenOrder, TRUE, "stop", FALSE)]_vars
=============================================================================
