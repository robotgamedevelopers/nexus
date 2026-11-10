-------------------------- MODULE WOverlayStranded --------------------------
\* Reversed witness: neither a frame nor the key check ever hides the overlay, so a show without the keyboard, or one
\* that lost it, stands over other apps for good. NeverStranded must reject it.
EXTENDS Overlay

BrokenObserve ==
    /\ UNCHANGED <<visible, active, armed>>
    /\ keyed' = (visible /\ (active \/ keyed))
    /\ Did("observe", "None")

BrokenKeyCheck ==
    /\ armed
    /\ armed' = FALSE
    /\ UNCHANGED <<visible, active>>
    /\ keyed' = IF visible THEN active ELSE keyed
    /\ Did("keycheck", "None")

BrokenSpec == Init /\ [][NextWith(Hotkey, Escape, BrokenObserve, BrokenKeyCheck)]_vars
    /\ WF_vars(BrokenObserve) /\ WF_vars(BrokenKeyCheck)
=============================================================================
