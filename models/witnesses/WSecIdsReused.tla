----------------------------- MODULE WSecIdsReused -----------------------------
\* Reversed witness: request ids are counted in memory and start again at 1 after a restart, so an answer typed into a
\* dialog from before the restart would satisfy a new, different request. ClosedStaysClosed must reject it.
EXTENDS MCSecrets

BrokenSpec == Init /\ [][NextWith(Ask, Sync, Receive, Timeout, RestartWith(LAMBDA n : 1))]_vars
=============================================================================
