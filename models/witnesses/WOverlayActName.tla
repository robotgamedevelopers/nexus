-------------------------- MODULE WOverlayActName --------------------------
\* Reversed witness: Escape answers an act the window does not know. TypeOK must reject it.
EXTENDS Overlay

BrokenEscape ==
    LET a == IF visible THEN "Hide" ELSE "None" IN
    /\ Carry(a, FALSE)
    /\ Arm(a)
    /\ keyed' = FALSE
    /\ last' = [op |-> "escape", act |-> "Close"]
    /\ UNCHANGED held

BrokenSpec == Init /\ [][NextWith(Hotkey, BrokenEscape, Observe, KeyCheck)]_vars
=============================================================================
