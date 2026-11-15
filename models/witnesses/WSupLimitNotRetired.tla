--------------------------- MODULE WSupLimitNotRetired ---------------------------
\* Reversed witness: a driver started without the limit is kept after the session is limited, and given the next
\* prompt (the retire drops its limit check). NeverWeaker must reject it.
EXTENDS MCSupervisor

Kept(x) == AdvanceAll(Take, LAMBDA y : RetireAll(Cleared, LAMBDA z : FALSE, y), WriteNext, x)

BrokenSpec == Init /\ [][NextAdvance(Kept)]_vars
=============================================================================
