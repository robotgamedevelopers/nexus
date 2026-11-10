---------------------------- MODULE WLinkNoNotice ----------------------------
\* Reversed witness: when entries left the tail before the window saw them, the mirror shows the tail without the
\* notice "N earlier entries not shown", so the window's transcript reads as if nothing was missed and its entries
\* stand at the wrong indices. ViewMatches must reject it.
EXTENDS MCLink

BrokenMirror(v, qv, se, s) ==
    LET r == MirrorTab(v, qv, se, s) IN
    [r EXCEPT !.view = v \o SubSeq(s.entries, Max(se.next, s.first) - s.first + 1, Len(s.entries))]

BrokenReceive(c, j) == ReceiveWith(BrokenMirror, c, j)

BrokenSpec == Init /\ [][NextWith(Deliver, BrokenReceive)]_vars
=============================================================================
