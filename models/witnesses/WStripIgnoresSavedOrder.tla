------------------------ MODULE WStripIgnoresSavedOrder ------------------------
\* Reversed witness: new tabs come in the list's order alone, the order the
\* person left the AIs in at the last quit lost. NewInSavedOrder must
\* reject it.
EXTENDS MCAIStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.saved = FALSE])]_vars
=============================================================================
