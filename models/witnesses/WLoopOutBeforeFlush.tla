------------------------ MODULE WLoopOutBeforeFlush ------------------------
\* Reversed witness: setState runs while the batch is still unflushed, changing linkSnap while batch is
\* non-empty. NoOutBeforeFlush must reject it.
EXTENDS MCLoopPass

BrokenOrder == <<"tick", "hold", "apply", "state", "flush", "release">>
BrokenSpec == Init /\ [][NextWith(BrokenOrder, TRUE, "stop", FALSE)]_vars
=============================================================================
