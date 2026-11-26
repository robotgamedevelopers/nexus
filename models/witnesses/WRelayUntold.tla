---------------------------- MODULE WRelayUntold ----------------------------
\* Reversed witness: the actions cut are kept and never handed to the window's poll. CutTold must reject it.
EXTENDS RelayLife

BrokenNext == Next /\ ~Tell
BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(Tick) /\ WF_vars(Start)
=============================================================================
