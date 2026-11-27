--------------------------- MODULE SessionGoalTrace ---------------------------
EXTENDS SessionGoal, SessionGoalTraceData, Sequences
VARIABLE cursor
allVars == <<cursor, vars>>
TraceInit == Init /\ cursor = 1
TraceNext == IF cursor < Len(Trace)
    THEN /\ cursor' = cursor + 1
         /\ LET r == Trace[cursor'] IN Step(r.event, r.now, r.pending)
    ELSE UNCHANGED allVars
TraceSpec == TraceInit /\ [][TraceNext]_allVars
Matches == /\ active = Trace[cursor].active /\ due = Trace[cursor].due /\ sends = Trace[cursor].sends
=============================================================================
