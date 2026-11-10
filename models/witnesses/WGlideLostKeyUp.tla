------------------------------ MODULE WGlideLostKeyUp ------------------------------
\* Reversed witness: the arrow's release (or the window losing focus) is lost, so the hold scrolls on for ever. Settles
\* must reject it.
EXTENDS MCGlide

BrokenSpec == Init /\ [][NextWith(Press, Key, Wheel, ReleaseWith(TRUE), Frame, Interrupt)]_vars
              /\ WF_vars(\E dt \in Dts : Frame(dt)) /\ WF_vars(ReleaseWith(TRUE))
=============================================================================
