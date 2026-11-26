---------------------------- MODULE WStripNoRestore ----------------------------
\* Reversed witness: the saved AI's tab arrives and is not selected, as a
\* window that judged the saved tab before the AIs came would.
\* RestoreSaved must reject it.
EXTENDS MCAIStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.restore = FALSE])]_vars
=============================================================================
