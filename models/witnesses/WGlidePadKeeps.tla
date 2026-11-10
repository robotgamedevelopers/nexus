------------------------------ MODULE WGlidePadKeeps ------------------------------
\* Reversed witness: a trackpad's scroll (or a press) leaves the motion running, so the next frame drags the page off
\* where the person put it. PadStops must reject it.
EXTENDS MCGlide

BrokenSpec == Init /\ [][NextWith(Press, Key, Wheel, Release, Frame, LAMBDA d, n : InterruptWith(FALSE, d, n))]_vars
=============================================================================
