--------------------------- MODULE WStripBlindRewrite ---------------------------
\* Reversed witness: a window that writes its own kept drafts over the shared file, not merging its change into
\* the file as it is then, loses a draft another window of the server kept. FileKeepsOthers must reject it.
EXTENDS MCAIStrip

BrokenSpec == Init /\ [][NextWith([Right EXCEPT !.merge = FALSE])]_vars
=============================================================================
