------------------------- MODULE WTakeoverNoRollback -------------------------
\* Reversed witness: the handover never puts the previous build back. A
\* placed build that never comes up on the store then leaves no server,
\* with the service restarting it forever. WorkingRuns must reject it.
EXTENDS Takeover

BrokenNext == Next /\ ~Rollback

BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(Ask) /\ WF_vars(Late) /\ WF_vars(DrainStop)
                   /\ WF_vars(Start) /\ WF_vars(StartFails) /\ WF_vars(HandedOver)
=============================================================================
