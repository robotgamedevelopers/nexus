------------------------- MODULE WLabelBeforeAtStart -------------------------
\* Reversed witness: the label to return to is taken from the title at the
\* first observation of a session, which ssh may already have set (the race
\* the first version of the code had). BeforeIsOwn must reject it.
EXTENDS TabLabel

BrokenObserve(r, t, late) ==
    IF r # NoRemote /\ remote = NoRemote
    THEN /\ Did(r, t, late)
         /\ remote' = r
         /\ before' = t
         /\ UNCHANGED <<after, masking, tail>>
    ELSE Observe(r, t, late)

BrokenSpec == Init /\ [][\E r \in Remotes \cup {NoRemote}, t \in Titles, late \in BOOLEAN :
                           BrokenObserve(r, t, late)]_vars
=============================================================================
