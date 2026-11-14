--------------------------- MODULE WSupSummaryLost ---------------------------
\* Reversed witness: the harness session made in another directory is dropped, but no summary is owed, so the new
\* driver starts with nothing of the conversation. SummaryOwed must reject it.
EXTENDS MCSupervisor

DropBare(x) == IF x.session # "" /\ x.sdir # x.dir THEN [x EXCEPT !.session = "", !.sdir = ""] ELSE x
Lost(x) == WriteNext(StartDriverWith(DropBare, Retire(Take(x))))

BrokenSpec == Init /\ [][NextAdvance(Lost)]_vars
=============================================================================
