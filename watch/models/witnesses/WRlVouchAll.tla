---------------------------- MODULE WRlVouchAll ----------------------------
\* Reversed witness: a restarted phone vouches for what its last run handed (no uncertain bound), and the server never got it. NoSilentLoss must reject it.
EXTENDS Relay

BrokenAnswer == [uncertain |-> 0, applied |-> acked]
BrokenReceive(r) ==
    /\ waiting' = waiting \o Taken(r)
    /\ resps' = IF Cardinality(resps) < MaxMsgs THEN resps \cup {BrokenAnswer} ELSE resps
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, handed, boot, acked, inflight, linkQ, applied, crashes>>

BrokenSpec == Init /\ [][Make \/ Send \/ Hand \/ Ack \/ Crash \/ (\E n \in 1..N : Deliver(n)) \/ (\E r \in reqs : BrokenReceive(r) \/ LoseReq(r))
                         \/ (\E m \in resps : Take(m) \/ LoseResp(m))]_vars
=============================================================================
