------------------------- MODULE WTakeoverDrainCuts -------------------------
\* Reversed witness: a draining server stops at once, its open turn cut before
\* the deadline. DrainCutsOnlyLate must reject it.
EXTENDS Takeover

BrokenDrainStop ==
    /\ server # None
    /\ draining
    /\ Gone
    /\ UNCHANGED <<placed, previous, places, service, handing, failed, rolled>>

BrokenNext == (Next /\ ~DrainStop) \/ BrokenDrainStop

BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================
