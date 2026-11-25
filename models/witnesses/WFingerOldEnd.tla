---------------------------- MODULE WFingerOldEnd ----------------------------
\* Reversed witness: the code before 2026-11-25: a change of range leaves a spring going to the old bottom, so it comes
\* to rest there, past the new one. SpringToItsEnd and AtRestInside must reject it (WFingerOldEnd.cfg,
\* WFingerOldEndRests.cfg).
EXTENDS MCFingerScroll

BrokenSpec == Init /\ [][NextWith(Down, Up, LAMBDA m : RangeWith(TRUE, TRUE, FALSE, m), Tick)]_vars
=============================================================================
