---------------------------- MODULE WStripReorders ----------------------------
\* Reversed witness: a list that removes a tab turns the tabs left round, as a
\* removal from the end that rebuilt the strip in its own order would.
\* SurvivorsKeepOrder must reject it.
EXTENDS MCFriendStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.order = FALSE])]_vars
=============================================================================
