-------------------------- MODULE WOverlayLossKept --------------------------
\* Reversed witness: a frame never hides the overlay, so after Command-Tab it stays up without the keyboard until the
\* key check. LossHides must reject it.
EXTENDS Overlay

BrokenObserve ==
    /\ UNCHANGED <<visible, active, armed>>
    /\ keyed' = (visible /\ (active \/ keyed))
    /\ Did("observe", "None")

BrokenSpec == Init /\ [][NextWith(Hotkey, Escape, BrokenObserve, KeyCheck)]_vars
=============================================================================
