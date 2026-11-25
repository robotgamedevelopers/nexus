---------------------------- MODULE WFingerCaughtTaps ----------------------------
\* Reversed witness: a press that never moved taps, even one that caught a moving list. CaughtIsNoTap must reject it.
EXTENDS MCFingerScroll

BrokenSpec == Init /\ [][NextWith(Down, UpWith(LAMBDA c : TRUE, LAMBDA c : FALSE), SetRange, Tick)]_vars
=============================================================================
