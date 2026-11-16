----------------------------- MODULE WLoopNoHold -----------------------------
\* Reversed witness: the batch starts without Drivers::hold(), so sends and stops go straight to the
\* live driver during Apply before Flush; a crash before Flush loses the batch and writes the prompt
\* a second time on reconnect. PromptAtMostOnce must reject it.
EXTENDS MCLoopPass

BrokenSpec == Init /\ [][NextWith(DefaultOrder, FALSE, "stop", FALSE)]_vars
=============================================================================
