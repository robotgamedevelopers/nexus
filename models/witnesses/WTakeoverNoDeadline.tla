------------------------- MODULE WTakeoverNoDeadline -------------------------
\* Reversed witness: a draining server waits for a moment with no turn open,
\* with no deadline. Turns that keep opening hold the old build running
\* forever. WorkingRuns must reject it.
EXTENDS Takeover

BrokenDrainStop ==
    /\ server # None
    /\ draining
    /\ ~turn
    /\ Gone
    /\ UNCHANGED <<placed, previous, places, service, handing, failed, rolled>>

BrokenNext == (Next /\ ~Late /\ ~DrainStop) \/ BrokenDrainStop

BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(Ask) /\ WF_vars(BrokenDrainStop) /\ WF_vars(Start)
                   /\ WF_vars(StartFails) /\ WF_vars(Rollback) /\ WF_vars(HandedOver)
=============================================================================
