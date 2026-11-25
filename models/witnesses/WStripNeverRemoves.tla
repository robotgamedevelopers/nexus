-------------------------- MODULE WStripNeverRemoves --------------------------
\* Reversed witness: the server's own list only adds tabs, as an older server's
\* sessions do, so an AI taken out of ais.json keeps its tab.
\* StripExact must reject it.
EXTENDS MCAIStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.remove = FALSE])]_vars
=============================================================================
