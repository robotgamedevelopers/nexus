--------------------------- MODULE WLinkMirrorAgain ---------------------------
\* Reversed witness: the window's mirror forgets which entries it showed and appends the whole tail each time it
\* receives a state, so a state received again, or a later one whose tail overlaps, shows entries twice. ViewMatches
\* must reject it.
EXTENDS MCLink

BrokenMirror(v, qv, se, s) ==
    LET r == MirrorTab(v, qv, se, s) IN
    [r EXCEPT !.view = v \o s.entries]

BrokenReceive(c, j) == ReceiveWith(BrokenMirror, c, j)

BrokenSpec == Init /\ [][NextWith(Deliver, BrokenReceive)]_vars
=============================================================================
