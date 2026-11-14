--------------------------- MODULE WSupAccountIgnored ---------------------------
\* Reversed witness: a driver started under the old account is kept after the account changes, and given the next
\* prompt. NoSendOnOldAccount must reject it.
EXTENDS MCSupervisor

Kept(x) == AdvanceAll(Take, LAMBDA y : y, WriteNext, x)

BrokenSpec == Init /\ [][NextAdvance(Kept)]_vars
=============================================================================
