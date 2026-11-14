--------------------------- MODULE WSupResumeElsewhere ---------------------------
\* Reversed witness: a driver started in another directory than its harness session's resumes it, where the harness
\* has no such session. ResumesItsDir must reject it.
EXTENDS MCSupervisor

NoDrop(x) == WriteNext(StartDriverWith(LAMBDA y : y, Retire(Take(x))))

BrokenSpec == Init /\ [][NextAdvance(NoDrop)]_vars
=============================================================================
