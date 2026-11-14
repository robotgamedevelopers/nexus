--------------------------- MODULE WSupSettingIgnored ---------------------------
\* Reversed witness: no settings line is ever written, so every send runs with the harness's defaults, whatever model
\* and effort the session has. The change never applies to a turn. WrittenWithSetting must reject it.
EXTENDS MCSupervisor

Ignored(x) == AdvanceAll(Take, Retire, LAMBDA y : WriteNextWith(LAMBDA z : z.told, y), x)

BrokenSpec == Init /\ [][NextAdvance(Ignored)]_vars
=============================================================================
