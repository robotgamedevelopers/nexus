------------------------------ MODULE WGlideClampTop ------------------------------
\* Reversed witness: the scroll is clamped at the bottom of the page but not at its top, so a key or a notch up near
\* the top sends the page above it. InRange must reject it.
EXTENDS MCGlide

NoTop(x) == IF x > Max THEN Max ELSE x

BrokenSpec == Init /\ [][NextWith(LAMBDA s : PressWith(NoTop, s), LAMBDA d : KeyWith(NoTop, d),
                                   LAMBDA d : WheelWith(NoTop, d), Release,
                                   LAMBDA dt : FrameWith(NoTop, SpringGo, dt), Interrupt)]_vars
=============================================================================
