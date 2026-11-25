---------------------------- MODULE WFingerFlingRestsPast ----------------------------
\* Reversed witness: a fling that slows to rest wherever its way takes it, past the end too, never springing.
\* AtRestInside must reject it.
EXTENDS MCFingerScroll

PastRest == /\ phase' = "rest" /\ off' \in Wide /\ Ahead(dir, off, off') /\ NoMotion

BrokenSpec == Init /\ [][NextWith(Down, Up, SetRange, TickWith(FlingTick \/ PastRest, SpringTick))]_vars
=============================================================================
