--------------------- MODULE WAccountMenuSidewaysKeeps ---------------------
\* Reversed witness: a sideways drag from the open menu keeps the menu, and the tab never moves. SidewaysIsTheTabsMove
\* must reject it.
EXTENDS AccountMenu

BrokenMove == \E at \in Places, way \in Ways :
    /\ chosen' = None
    /\ IF phase = "open"
         THEN /\ phase' = "open" /\ hovered' = IF at \in Rows THEN at ELSE None /\ UNCHANGED moving
         ELSE UNCHANGED <<phase, hovered, moving>>
    /\ Did(IF way = "sideways" THEN "side" ELSE "move", at)

BrokenSpec == Init /\ [][NextWith(Press, BrokenMove, Lift)]_vars
=============================================================================
