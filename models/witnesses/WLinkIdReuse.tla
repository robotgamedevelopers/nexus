----------------------------- MODULE WLinkIdReuse -----------------------------
\* Reversed witness: a restart forgets the id counter (as if it were memory only, or recomputed from the sessions the
\* store held at the start), so the next create mints an id the server already holds. IdsNeverReused must reject it.
EXTENDS MCLink

BrokenRestart == RestartAll(CutLost, Len(Start) + 1)

BrokenSpec == Init /\ [][NextWithAll(Deliver, Receive, Close, Take, BrokenRestart)]_vars
=============================================================================
