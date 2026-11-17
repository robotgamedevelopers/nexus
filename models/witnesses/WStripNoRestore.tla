---------------------------- MODULE WStripNoRestore ----------------------------
\* Reversed witness: the saved friend's tab arrives and is not selected, as a
\* window that judged the saved tab before the friends came would.
\* RestoreSaved must reject it.
EXTENDS MCFriendStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.restore = FALSE])]_vars
=============================================================================
