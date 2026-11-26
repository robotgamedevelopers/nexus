---------------------------- MODULE WLinkMirrorOpen ----------------------------
\* Reversed witness: the window's mirror appends the AI's open entry to its transcript, as if it were closed, so
\* the window shows text that is still growing, and shows it again once it is closed. ViewMatches must reject it.
EXTENDS MCLink

BrokenMirror(v, qv, se, s) ==
    LET r == MirrorTab(v, qv, se, s) IN
    [r EXCEPT !.view = IF r.live = None THEN @ ELSE Append(@, r.live)]

BrokenReceive(c, j) == ReceiveWith(BrokenMirror, c, j)

BrokenSpec == Init /\ [][NextWith(Deliver, BrokenReceive)]_vars
=============================================================================
