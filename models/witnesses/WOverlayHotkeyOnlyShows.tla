---------------------- MODULE WOverlayHotkeyOnlyShows ----------------------
\* Reversed witness: the hotkey always shows, so it never hides an overlay that is up with the keyboard. HotkeyHides
\* must reject it.
EXTENDS Overlay

BrokenHotkey ==
    /\ \E granted \in BOOLEAN : Carry("Show", granted)
    /\ Arm("Show")
    /\ keyed' = FALSE
    /\ Did("hotkey", "Show")

BrokenSpec == Init /\ [][NextWith(BrokenHotkey, Escape, Observe, KeyCheck)]_vars
=============================================================================
