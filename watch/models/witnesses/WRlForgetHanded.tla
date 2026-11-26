---------------------------- MODULE WRlForgetHanded ----------------------------
\* Reversed witness: the phone keeps nothing across a restart, so it hands a prompt again. AtMostOnce must reject it.
EXTENDS Relay

BrokenCrash ==
    /\ crashes < MaxCrash /\ crashes' = crashes + 1
    /\ handed' = acked /\ boot' = acked /\ acked' = acked /\ waiting' = << >> /\ inflight' = << >>
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, resps, linkQ, applied>>

BrokenSpec == Init /\ [][(Next /\ ~Crash) \/ BrokenCrash]_vars
=============================================================================
