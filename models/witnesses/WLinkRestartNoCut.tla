--------------------------- MODULE WLinkRestartNoCut ---------------------------
\* Reversed witness: a restart drops the AI's open entry without the notice that cuts its turn, so a window that
\* showed the text growing holds text the server no longer has anywhere. LiveSound must reject it.
EXTENDS MCLink

Dropped(t) == tr[t]
BrokenRestart == RestartWith(Dropped)

BrokenSpec == Init /\ [][NextWithAll(Deliver, Receive, Close, Take, BrokenRestart)]_vars
=============================================================================
