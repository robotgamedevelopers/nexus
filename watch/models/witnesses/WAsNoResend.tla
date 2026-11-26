---------------------------- MODULE WAsNoResend ----------------------------
\* Reversed witness: a restarted phone still remembers the newest choice it took (as if `seen` were kept), so it never takes the watch's held choice again, which its old run never handed. ChoiceLeaves must reject it.
EXTENDS AccountSwitch

BrokenCrash ==
    /\ crashes < MaxCrash
    /\ crashes' = crashes + 1
    /\ waiting' = None /\ inflight' = None /\ runQ' = {} /\ switched' = 0 /\ view' = ""
    /\ UNCHANGED <<server, applied, chosen, choice, shown, reqs, resps, linkQ, seen>>

BrokenNext ==
    \/ \E a \in Accounts : Choose(a)
    \/ Send \/ Hand \/ Look \/ Ack \/ BrokenCrash \/ RoundTrip
    \/ \E r \in reqs : Receive(r) \/ LoseReq(r)
    \/ \E m \in resps : Take(m) \/ LoseResp(m)
    \/ \E x \in linkQ : Deliver(x)

BrokenSpec == Init /\ [][BrokenNext]_vars
              /\ WF_vars(RoundTrip) /\ WF_vars(Hand) /\ WF_vars(Look) /\ WF_vars(DeliverCurrent) /\ WF_vars(Ack)
=============================================================================
