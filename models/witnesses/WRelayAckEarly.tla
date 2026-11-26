---------------------------- MODULE WRelayAckEarly ----------------------------
\* Reversed witness: the window counts as acknowledged what it wrote to the relay, not what the server applied.
\* AckTrue must reject it.
EXTENDS RelayLife

BrokenAck ==
    /\ phase = "running"
    /\ \E m \in base .. written : m >= ackHigh /\ ackHigh' = m
    /\ UNCHANGED <<phase, procs, gen, left, backoff, sent, written, base, cut, told, wr, ap>>
BrokenNext == (Next /\ ~Ack) \/ BrokenAck
BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================
