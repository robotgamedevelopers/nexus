---------------------------- MODULE WFingerSpringForever ----------------------------
\* Reversed witness: a spring that never comes to rest: it swings on and out again past its end, frame after frame.
\* Settles must reject it.
EXTENDS MCFingerScroll

Swings == /\ off' \in Wide /\ Outside(side, send, off') /\ stage' \in {"out", "back"}
          /\ UNCHANGED <<phase, dir, send, side>>
BrokenTick == TickWith(FlingTick, Swings)

BrokenSpec == Init /\ [][NextWith(Down, Up, SetRange, BrokenTick)]_vars /\ WF_vars(BrokenTick)
=============================================================================
