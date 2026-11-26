---------------------------- MODULE WRelayStuck ----------------------------
\* Reversed witness: the backoff never runs out (a port whose wait is never woken). Restarts must reject it.
EXTENDS RelayLife

BrokenNext == Next /\ ~Tick
BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(Start) /\ WF_vars(Tell)
=============================================================================
