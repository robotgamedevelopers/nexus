--------------------- MODULE WAccountMenuLiftKeepsOpen ---------------------
\* Reversed witness: a lift off every row leaves the menu open for a tap (Glenn: "not tap-tap"). LiftCloses must
\* reject it.
EXTENDS AccountMenu

BrokenLift == \E at \in Places :
    /\ phase' = IF phase = "open" /\ at \notin Rows THEN "open" ELSE "closed"
    /\ hovered' = None
    /\ chosen' = IF phase = "open" /\ at \in Rows THEN at ELSE None
    /\ moving' = FALSE
    /\ Did("lift", at)

BrokenSpec == Init /\ [][NextWith(Press, Move, BrokenLift)]_vars
=============================================================================
