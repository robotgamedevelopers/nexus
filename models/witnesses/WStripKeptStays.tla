---------------------------- MODULE WStripKeptStays ----------------------------
\* Reversed witness: a kept draft given back to the friend's new tab stays
\* kept too, so the next removal could put an older draft over a newer one.
\* KeptOnlyAbsent must reject it.
EXTENDS MCFriendStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.erase = FALSE])]_vars
=============================================================================
