----------------------- MODULE WOverlayEscapeIgnored -----------------------
\* Reversed witness: Escape hides nothing. EscapeHides must reject it.
EXTENDS Overlay

BrokenEscape ==
    /\ UNCHANGED <<visible, active, armed, keyed>>
    /\ Did("escape", "None")

BrokenSpec == Init /\ [][NextWith(Hotkey, BrokenEscape, Observe, KeyCheck)]_vars
=============================================================================
