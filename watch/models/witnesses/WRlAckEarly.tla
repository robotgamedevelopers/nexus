---------------------------- MODULE WRlAckEarly ----------------------------
\* Reversed witness: the relay counts a prompt done once handed, before the server has it. DroppedWasApplied must reject it.
EXTENDS Relay

BrokenAck ==
    /\ inflight # << >>
    /\ acked' = Max({acked} \cup Range(inflight)) /\ inflight' = << >>
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, resps, handed, boot, waiting, linkQ, applied, crashes>>

BrokenSpec == Init /\ [][Next \/ BrokenAck]_vars
=============================================================================
