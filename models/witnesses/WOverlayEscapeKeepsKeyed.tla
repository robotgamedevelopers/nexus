---------------------- MODULE WOverlayEscapeKeepsKeyed ----------------------
\* Reversed witness: Escape hides the overlay but keeps the note that it had the keyboard. KeyedOnlyVisible must
\* reject it.
EXTENDS Overlay

BrokenEscape ==
    LET a == IF visible THEN "Hide" ELSE "None" IN
    /\ Carry(a, FALSE)
    /\ Arm(a)
    /\ UNCHANGED keyed
    /\ Did("escape", a)

BrokenSpec == Init /\ [][NextWith(Hotkey, BrokenEscape, Observe, KeyCheck)]_vars
=============================================================================
