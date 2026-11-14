------------------------- MODULE WSupSettingNotCleared -------------------------
\* Reversed witness: a driver told a model is kept after the session puts the model back to the harness's default.
\* A settings line cannot clear a field, so the next turn runs with the old model. WrittenWithSetting must reject it.
EXTENDS MCSupervisor

Kept(x) == AdvanceAll(Take, LAMBDA y : RetireWith(LAMBDA z : FALSE, y), WriteNext, x)

BrokenSpec == Init /\ [][NextAdvance(Kept)]_vars
=============================================================================
