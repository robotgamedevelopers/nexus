--------------------------- MODULE WSupPlaceIgnored ---------------------------
\* Reversed witness: a driver started in the old directory is kept after the session's instance moves, and given the
\* next prompt there. NoSendInOldPlace must reject it.
EXTENDS MCSupervisor

RetireNotMoved(x) ==
    IF x.phase = "idle" /\ (x.drv # x.account \/ Cleared(x)) THEN [QuietEnd(x) EXCEPT !.retiring = TRUE] ELSE x
Kept(x) == AdvanceAll(Take, RetireNotMoved, WriteNext, x)

BrokenSpec == Init /\ [][NextAdvance(Kept)]_vars
=============================================================================
