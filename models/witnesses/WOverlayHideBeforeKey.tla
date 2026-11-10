----------------------- MODULE WOverlayHideBeforeKey -----------------------
\* Reversed witness: a frame hides any shown overlay without the keyboard, so one just shown is hidden before the
\* keyboard arrives. NoHideBeforeKey must reject it.
EXTENDS Overlay

BrokenObserve ==
    LET a == IF visible /\ ~active THEN "Hide" ELSE "None" IN
    /\ Carry(a, FALSE)
    /\ Arm(a)
    /\ keyed' = (visible /\ active)
    /\ Did("observe", a)

BrokenSpec == Init /\ [][NextWith(Hotkey, Escape, BrokenObserve, KeyCheck)]_vars
=============================================================================
