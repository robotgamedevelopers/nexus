---------------------------- MODULE WLabelNoTail ----------------------------
\* Reversed witness: no tail, so a last title the session sets after its
\* process is seen gone shows on the tab. TailRestores must reject it.
EXTENDS TabLabel

BrokenObserve(r, t, late) ==
    IF r = NoRemote /\ remote = NoRemote /\ masking
    THEN /\ Did(r, t, late)
         /\ tail' = FALSE
         /\ masking' = (t = after)
         /\ after' = IF t = after THEN after ELSE ""
         /\ before' = IF t = after THEN before ELSE t
         /\ UNCHANGED remote
    ELSE Observe(r, t, late)

BrokenSpec == Init /\ [][\E r \in Remotes \cup {NoRemote}, t \in Titles, late \in BOOLEAN :
                           BrokenObserve(r, t, late)]_vars
=============================================================================
