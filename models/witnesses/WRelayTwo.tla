---------------------------- MODULE WRelayTwo ----------------------------
\* Reversed witness: the next relay starts as soon as the backoff is over, without waiting for the last one's
\* process to exit (a port that tells RelayLife the relay ended when its output closes). OneRelay must reject it.
EXTENDS RelayLife

BrokenStart == StartWith(phase # "stopped" /\ left = 0)
BrokenNext == (Next /\ ~Start) \/ BrokenStart
BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================
