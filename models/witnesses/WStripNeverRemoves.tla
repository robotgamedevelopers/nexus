-------------------------- MODULE WStripNeverRemoves --------------------------
\* Reversed witness: the server's own list only adds tabs, as an older server's
\* sessions do, so a friend taken out of friends.json keeps its tab.
\* StripExact must reject it.
EXTENDS MCFriendStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.remove = FALSE])]_vars
=============================================================================
