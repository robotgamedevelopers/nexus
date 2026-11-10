-------------------------- MODULE WLabelMaskSticks --------------------------
\* Reversed witness: once a session ends the mask never lifts, so the tab
\* keeps the label from before whatever the shell sets later. MaskEnds must
\* reject it.
EXTENDS TabLabel

BrokenObserve(r, t, late) ==
    IF r = NoRemote /\ remote = NoRemote /\ masking
    THEN /\ Did(r, t, late)
         /\ tail' = (tail /\ ~late)
         /\ after' = IF tail /\ ~late THEN t ELSE after
         /\ UNCHANGED <<remote, before, masking>>
    ELSE Observe(r, t, late)

BrokenSpec == Init /\ [][\E r \in Remotes \cup {NoRemote}, t \in Titles, late \in BOOLEAN :
                           BrokenObserve(r, t, late)]_vars
=============================================================================
