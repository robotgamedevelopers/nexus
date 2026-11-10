---------------------- MODULE WOverlayHideWhileActive ----------------------
\* Reversed witness: a frame hides an overlay that has had the keyboard whatever it has now, as if anything else on
\* screen (an input method's window, the emoji picker) counted as a loss. NoHideWhileActive must reject it.
EXTENDS Overlay

BrokenObserve ==
    LET a == IF visible /\ keyed THEN "Hide" ELSE "None" IN
    /\ Carry(a, FALSE)
    /\ Arm(a)
    /\ keyed' = (visible /\ active /\ a = "None")
    /\ Did("observe", a)

BrokenSpec == Init /\ [][NextWith(Hotkey, Escape, BrokenObserve, KeyCheck)]_vars
=============================================================================
