----------------------- MODULE WTakeoverServiceStarts -----------------------
\* Reversed witness: only the service starts the placed build, as setup did
\* before the handover process. Without a service (--no-service, WSL2
\* without systemd), the old server drains and nothing starts the new one.
\* WorkingRuns must reject it.
EXTENDS Takeover

BrokenStart ==
    /\ server = None
    /\ service
    /\ placed \notin Broken
    /\ server' = placed
    /\ UNCHANGED <<placed, previous, draining, late, turn, places, service, handing, failed, rolled>>

BrokenNext == (Next /\ ~Start) \/ BrokenStart

BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(Ask) /\ WF_vars(Late) /\ WF_vars(DrainStop)
                   /\ WF_vars(BrokenStart) /\ WF_vars(StartFails) /\ WF_vars(Rollback) /\ WF_vars(HandedOver)
=============================================================================
