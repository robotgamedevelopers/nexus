------------------------------ MODULE WGlideSpringInside ------------------------------
\* Reversed witness: an underdamped spring, which swings back past its end into the page. SpringAtEnd must reject it.
EXTENDS MCGlide

Swing(l) == /\ l > 0 /\ left' = l
            /\ pos' \in Range /\ over' = 0

BrokenSpec == Init /\ [][NextWith(Press, Key, Wheel, Release, LAMBDA dt : FrameWith(Clamp, Swing, dt), Interrupt)]_vars
=============================================================================
