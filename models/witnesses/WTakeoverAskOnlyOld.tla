------------------------- MODULE WTakeoverAskOnlyOld -------------------------
\* Reversed witness: the installer acts only on a server from before
\* takeover, taking any build that answers `build` as current. A rebuild
\* then never runs. PlacedRuns must reject it.
EXTENDS Takeover

BrokenAsk == Ask /\ server = OldBuild

BrokenNext == (Next /\ ~Ask) \/ BrokenAsk

BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(BrokenAsk) /\ WF_vars(Late) /\ WF_vars(DrainStop)
                   /\ WF_vars(Start)
=============================================================================
