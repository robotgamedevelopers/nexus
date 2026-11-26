---------------------- MODULE WAccountMenuDecidedMoves ----------------------
\* Reversed witness: a slide sideways across a wide row, after the finger went down onto the menu, still closes the menu
\* and moves the tab (no deciding). DecidedStaysTheMenu must reject it.
EXTENDS AccountMenu

BrokenMove == \E at \in Places, way \in Ways :
    /\ chosen' = None
    /\ IF phase = "open"
         THEN IF way = "sideways"
                THEN /\ phase' = "tabmove" /\ hovered' = None /\ moving' = TRUE /\ UNCHANGED decided
                ELSE /\ phase' = "open" /\ hovered' = (IF at \in Rows THEN at ELSE None) /\ UNCHANGED moving
                     /\ decided' = (decided \/ way = "down")
         ELSE UNCHANGED <<phase, hovered, moving, decided>>
    /\ Did(IF way = "sideways" THEN "side" ELSE "move", at)

BrokenSpec == Init /\ [][NextWith(Press, BrokenMove, Lift)]_vars
=============================================================================
