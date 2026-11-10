--------------------- MODULE WOverlayHotkeyHidesUnkeyed ---------------------
\* Reversed witness: the hotkey hides any visible overlay, with the keyboard or without, so a press after the overlay
\* lost the keyboard hides it instead of showing it. HotkeyShows must reject it.
EXTENDS Overlay

BrokenHotkey ==
    LET a == IF visible THEN "Hide" ELSE "Show" IN
    /\ \E granted \in BOOLEAN : Carry(a, granted)
    /\ Arm(a)
    /\ keyed' = FALSE
    /\ Did("hotkey", a)

BrokenSpec == Init /\ [][NextWith(BrokenHotkey, Escape, Observe, KeyCheck)]_vars
=============================================================================
