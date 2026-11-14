--------------------------- MODULE WLinkShowNewest ---------------------------
\* Reversed witness: a window's tab shows its friend's newest session in each state it receives, not the first one it
\* showed, so a session made for the friend moves the tab off the transcript it showed. ShowsFirst must reject it.
EXTENDS MCLink

Newest(cur, s, f) ==
    IF \E i \in Ids : s[i].friend = f
    THEN CHOOSE i \in Ids : s[i].friend = f /\ \A k \in Ids : s[k].friend = f => k <= i
    ELSE cur

BrokenReceive(c, j) == ReceiveAll(MirrorTab, Newest, c, j)

BrokenSpec == Init /\ [][NextWith(Deliver, BrokenReceive)]_vars
=============================================================================
