---------------------------- MODULE WStripBadName ----------------------------
\* Reversed witness: every name the server sends gets a tab, one that is not an
\* AI's name too, as a window that trusted the server's names would.
\* NamesValid must reject it.
EXTENDS MCAIStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.valid = FALSE])]_vars
=============================================================================
