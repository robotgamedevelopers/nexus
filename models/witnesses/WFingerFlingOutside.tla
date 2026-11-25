------------------------- MODULE WFingerFlingOutside -------------------------
\* Reversed witness: the code before 2026-11-25: a fling the range shrank under goes on past the new bottom until its
\* next frame (which then jumped it to the bottom). FlingInside must reject it.
EXTENDS MCFingerScroll

BrokenSpec == Init /\ [][NextWith(Down, Up, LAMBDA m : RangeWith(TRUE, FALSE, TRUE, m), Tick)]_vars
=============================================================================
