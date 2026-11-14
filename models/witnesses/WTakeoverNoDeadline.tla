------------------------- MODULE WTakeoverNoDeadline -------------------------
\* Reversed witness: a draining server waits for a moment with no turn open,
\* with no deadline. Turns that keep opening hold the old build running
\* forever. PlacedRuns must reject it.
EXTENDS Takeover

BrokenDrainStop ==
    /\ server # None
    /\ draining
    /\ ~turn
    /\ Gone
    /\ UNCHANGED <<placed, places>>

BrokenNext == (Next /\ ~Late /\ ~DrainStop) \/ BrokenDrainStop

BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(Ask) /\ WF_vars(BrokenDrainStop) /\ WF_vars(Start)
=============================================================================
