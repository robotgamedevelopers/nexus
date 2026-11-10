----------------------- MODULE WOverlayShowUnchecked -----------------------
\* Reversed witness: the hotkey's show does not arm the key check. UnkeyedIsChecked must reject it.
EXTENDS Overlay

BrokenHotkey ==
    LET a == IF visible /\ active THEN "Hide" ELSE "Show" IN
    /\ \E granted \in BOOLEAN : Carry(a, granted)
    /\ IF a = "Show" THEN UNCHANGED armed ELSE Arm(a)
    /\ keyed' = FALSE
    /\ Did("hotkey", a)

BrokenSpec == Init /\ [][NextWith(BrokenHotkey, Escape, Observe, KeyCheck)]_vars
=============================================================================
