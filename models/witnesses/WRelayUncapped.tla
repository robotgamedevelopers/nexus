---------------------------- MODULE WRelayUncapped ----------------------------
\* Reversed witness: the backoff doubles with no cap. BackoffOK must reject it.
EXTENDS RelayLife

Uncapped(wait) == 2 * wait
BrokenDeath == DeathWith(Lost, written, Uncapped)
BrokenNext == (Next /\ ~Death) \/ BrokenDeath
BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================
