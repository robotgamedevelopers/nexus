---------------------------- MODULE WRelaySilent ----------------------------
\* Reversed witness: a relay that ends takes its unacknowledged actions with it and says nothing (the port before
\* RelayLife). Covered must reject it.
EXTENDS RelayLife

BrokenDeath == DeathWith({}, written, Doubled)
BrokenNext == (Next /\ ~Death) \/ BrokenDeath
BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================
