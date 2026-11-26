---------------------------- MODULE WRelayNoBackoff ----------------------------
\* Reversed witness: a relay that ends is started again at once, with no backoff: a relay that cannot start (no
\* nexus-link-relay in WSL2) would be started in a tight loop. WaitsBackoff must reject it.
EXTENDS RelayLife

BrokenStart == StartWith(phase = "wait" /\ procs = 0)
BrokenNext == (Next /\ ~Start) \/ BrokenStart
BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================
