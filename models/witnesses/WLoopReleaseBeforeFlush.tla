----------------------- MODULE WLoopReleaseBeforeFlush -----------------------
\* Reversed witness: Drivers::release runs before DurableStore::flush, so a prompt is written to a driver
\* before its batch is durable; a crash loses the batch, the reconnecting window sends the action again,
\* and the prompt is written to a driver a second time. PromptAtMostOnce must reject it.
EXTENDS MCLoopPass

BrokenOrder == <<"tick", "hold", "apply", "release", "flush", "state">>
BrokenSpec == Init /\ [][NextWith(BrokenOrder, TRUE, "stop", FALSE)]_vars
=============================================================================
