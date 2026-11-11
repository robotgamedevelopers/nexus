-------------------------- MODULE WLinkCloseKeepsOpen --------------------------
\* Reversed witness: the server closes the friend's open entry into the transcript but keeps showing it open, so the
\* same text is in the transcript and still growing. EachClosedOnce must reject it.
EXTENDS MCLink

BrokenClose(t) ==
    /\ op[t] # NoOpen
    /\ tr' = [tr EXCEPT ![t] = Append(@, Grown(op[t]))]
    /\ snaps' = Push(tr', q, op)
    /\ UNCHANGED <<acts, applied, done, q, op, stops, made, last, view, vq, seen, live>>

BrokenSpec == Init /\ [][NextWithAll(Deliver, Receive, BrokenClose, Take, Restart)]_vars
=============================================================================
