------------------------- MODULE WOverlayRefusedKept -------------------------
\* Reversed witness: the key check never hides, so a show the OS answers without the keyboard stays up over other
\* apps. RefusedShowHides must reject it.
EXTENDS Overlay

BrokenKeyCheck ==
    /\ armed
    /\ armed' = FALSE
    /\ UNCHANGED <<visible, active>>
    /\ keyed' = IF visible THEN active ELSE keyed
    /\ Did("keycheck", "None")

BrokenSpec == Init /\ [][NextWith(Hotkey, Escape, Observe, BrokenKeyCheck)]_vars
=============================================================================
