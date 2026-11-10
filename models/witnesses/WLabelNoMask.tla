---------------------------- MODULE WLabelNoMask ----------------------------
\* Reversed witness: a session's end shows the title it left, as a terminal
\* that reads only the program's title would. EndRestores must reject it.
EXTENDS TabLabel

BrokenObserve(r, t, late) ==
    IF r = NoRemote /\ remote # NoRemote
    THEN /\ Did(r, t, late)
         /\ remote' = NoRemote
         /\ masking' = FALSE
         /\ tail' = FALSE
         /\ after' = ""
         /\ before' = t
    ELSE Observe(r, t, late)

BrokenSpec == Init /\ [][\E r \in Remotes \cup {NoRemote}, t \in Titles, late \in BOOLEAN :
                           BrokenObserve(r, t, late)]_vars
=============================================================================
