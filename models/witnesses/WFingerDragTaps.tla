---------------------------- MODULE WFingerDragTaps ----------------------------
\* Reversed witness: an up after a drag taps the row under it, as a press that never moved does. DragIsNoTap must reject
\* it.
EXTENDS MCFingerScroll

BrokenSpec == Init /\ [][NextWith(Down, UpWith(LAMBDA c : ~c, LAMBDA c : ~c), SetRange, Tick)]_vars
=============================================================================
