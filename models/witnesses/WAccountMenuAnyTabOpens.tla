---------------------- MODULE WAccountMenuAnyTabOpens ----------------------
\* Reversed witness: a press on another AI's tab opens the menu too. OpensOnlyOnTheActiveTab must reject it.
EXTENDS AccountMenu

BrokenPress == \E at \in Places :
    /\ phase' = IF phase # "open" /\ at \in {"active", "other"} THEN "open" ELSE "closed"
    /\ hovered' = None
    /\ chosen' = None
    /\ moving' = FALSE
    /\ decided' = FALSE
    /\ Did("press", at)

BrokenSpec == Init /\ [][NextWith(BrokenPress, Move, Lift)]_vars
=============================================================================
