-------------------------- MODULE WReconnectGivesUp --------------------------
\* Reversed witness: a backoff that never ends: once an attempt has started
\* one, time never brings the next attempt (only hearing a server would).
\* After a server stops, the window's first attempt can find no server yet,
\* and it never tries again. Reconnects must reject it.
EXTENDS Reconnect

BrokenTime == TimeWith({due})

BrokenNext == (Next /\ ~Time) \/ BrokenTime

BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(Dial) /\ WF_vars(Hear) /\ WF_vars(BrokenTime)
=============================================================================
