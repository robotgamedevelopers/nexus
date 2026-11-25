---------------------------- MODULE WFingerSpringInside ----------------------------
\* Reversed witness: an underdamped spring, which swings back past its end into the list. SpringNeverCrossesInside must
\* reject it.
EXTENDS MCFingerScroll

Swing == off' \in Wide /\ UNCHANGED <<phase, dir, send, side, stage>>

BrokenSpec == Init /\ [][NextWith(Down, Up, SetRange, TickWith(FlingTick, SpringTick \/ Swing))]_vars
=============================================================================
