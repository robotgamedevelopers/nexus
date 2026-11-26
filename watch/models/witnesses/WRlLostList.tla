---------------------------- MODULE WRlLostList ----------------------------
\* Reversed witness: the answer lists the frame's own prompts it cannot vouch for, rather than a bound: taken against a newer outbox, it vouches for a prompt it was never asked about. DroppedWasApplied must reject it.
EXTENDS Relay

ListAnswer(r) == [lost |-> {n \in r : n <= boot}, applied |-> acked]
ListReceive(r) ==
    /\ waiting' = waiting \o Taken(r)
    /\ resps' = IF Cardinality(resps) < MaxMsgs THEN resps \cup {ListAnswer(r)} ELSE resps
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, handed, boot, acked, inflight, linkQ, applied, crashes>>
ListTake(m) ==
    LET doubt == outbox \cap m.lost
        done == {n \in outbox : n <= m.applied} \ m.lost
    IN /\ outbox' = outbox \ (doubt \cup done)
       /\ told' = told \cup doubt /\ dropped' = dropped \cup done
       /\ UNCHANGED <<made, reqs, resps, phoneVars, linkQ, applied, crashes>>

BrokenSpec == Init /\ [][Make \/ Send \/ Hand \/ Ack \/ Crash \/ (\E n \in 1..N : Deliver(n)) \/ (\E r \in reqs : ListReceive(r) \/ LoseReq(r))
                         \/ (\E m \in resps : ListTake(m) \/ LoseResp(m))]_vars
=============================================================================
