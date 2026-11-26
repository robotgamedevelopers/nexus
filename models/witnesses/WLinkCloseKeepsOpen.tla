-------------------------- MODULE WLinkCloseKeepsOpen --------------------------
\* Reversed witness: the server closes the AI's open entry into the transcript but keeps showing it open, so the
\* same text is in the transcript and still growing. EachClosedOnce must reject it.
EXTENDS MCLink

BrokenClose(i) ==
    /\ op[i] # NoOpen
    /\ tr' = [tr EXCEPT ![i] = Append(@, Grown(op[i]))]
    /\ snaps' = Push(ses, tr', q, op)
    /\ UNCHANGED <<acts, applied, done, ses, nextSid, minted, skipped, q, op, stops, made, window>>

BrokenSpec == Init /\ [][NextWithAll(Deliver, Receive, BrokenClose, Take, Restart)]_vars
=============================================================================
