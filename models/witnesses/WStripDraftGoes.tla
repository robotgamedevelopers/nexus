--------------------------- MODULE WStripDraftGoes ---------------------------
\* Reversed witness: a removed friend's draft goes with its tab, as the window's
\* first version of the server's list had it. DraftsKept must reject it.
EXTENDS MCFriendStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.keep = FALSE])]_vars
=============================================================================
