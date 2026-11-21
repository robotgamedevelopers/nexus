------------------------ MODULE AccountPickerTrace ------------------------
EXTENDS AccountPicker, AccountPickerTraceData
VARIABLE cursor
allVars == <<cursor, vars>>
Take(r) == CASE r.op = "open" -> Open(r.tab, r.session, r.options, r.current, r.direction, r.ready)
             [] r.op = "step" -> Step(r.direction)
             [] r.op = "refresh" -> Refresh(r.tab, r.session, r.options, r.current)
             [] r.op \in {"cancel", "lost", "modal"} -> Cancel
             [] r.op = "confirm" -> Choose(picker.highlight)
             [] r.op = "row" -> Choose(r.id)
TraceInit == Init /\ cursor = 1
TraceNext == IF cursor < Len(Trace) THEN /\ cursor' = cursor + 1 /\ Take(Trace[cursor'])
             ELSE UNCHANGED allVars
TraceSpec == TraceInit /\ [][TraceNext]_allVars
Matches == picker = Trace[cursor].picker /\ request = Trace[cursor].request
=============================================================================
