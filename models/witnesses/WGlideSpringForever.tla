------------------------------ MODULE WGlideSpringForever ------------------------------
\* Reversed witness: a spring whose time never runs out, which may stay past its end for ever. Settles must reject it.
EXTENDS MCGlide

Forever(l) == /\ left' = left
              /\ pos' = target
              /\ over' \in (1 - Bound) .. (Bound - 1) /\ (IF target = Max THEN over' >= 0 ELSE over' <= 0)

BrokenFrame(dt) == FrameWith(Clamp, Forever, dt)

BrokenSpec == Init /\ [][NextWith(Press, Key, Wheel, Release, BrokenFrame, Interrupt)]_vars
              /\ WF_vars(\E dt \in Dts : BrokenFrame(dt)) /\ WF_vars(Release)
=============================================================================
