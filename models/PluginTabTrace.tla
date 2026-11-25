--------------------------- MODULE PluginTabTrace ---------------------------
EXTENDS PluginTabs, PluginTabTraceData
VARIABLE step
allVars == <<step, vars>>
Take(r) == CASE r.op = "open" -> Open(r.a)
             [] r.op = "close" -> Close(r.a)
             [] r.op = "reload" -> Reload(r.a = 1)
             [] r.op = "move" -> IF Permits(groups[tabs[r.a]], groups[tabs[r.b]]) THEN Move(r.a, r.b) ELSE UNCHANGED vars
NextStep == IF step < Len(Trace) THEN /\ step' = step + 1 /\ Take(Trace[step'])
                                 ELSE UNCHANGED allVars
TraceSpec == (Init /\ step = 1) /\ [][NextStep]_allVars
Matches == /\ tabs = Trace[step].tabs
           /\ [i \in DOMAIN tabs |-> groups[tabs[i]]] = Trace[step].groups
=============================================================================
