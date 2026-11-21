--------------------------- MODULE PluginTabTrace ---------------------------
EXTENDS PluginTabs, PluginTabTraceData
VARIABLE step
vars == <<step, tabs>>
Take(r) == CASE r.op = "open" -> Open(r.a)
             [] r.op = "close" -> tabs' = Drop(tabs, r.a)
             [] r.op = "move" -> IF Group(tabs[r.a]) = Group(tabs[r.b]) THEN Move(r.a, r.b) ELSE UNCHANGED tabs
NextStep == IF step < Len(Trace) THEN /\ step' = step + 1 /\ Take(Trace[step'])
                                 ELSE UNCHANGED vars
TraceSpec == (Init /\ step = 1) /\ [][NextStep]_vars
Matches == tabs = Trace[step].tabs
=============================================================================
