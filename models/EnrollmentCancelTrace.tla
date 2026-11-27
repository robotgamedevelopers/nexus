----------------------- MODULE EnrollmentCancelTrace -----------------------
EXTENDS EnrollmentCancel, EnrollmentCancelTraceData, Sequences
VARIABLE cursor
allVars == <<cursor, vars>>
TraceInit == Init /\ cursor = 1
TraceNext == /\ cursor' = IF cursor < Len(Trace) THEN cursor + 1 ELSE cursor
             /\ UNCHANGED vars
TraceSpec == TraceInit /\ [][TraceNext]_allVars
Matches == LET r == Trace[cursor] IN
    /\ r.after = Result(r.before, r.matching, r.owner, r.nonempty)
    /\ r.drained = Drained(r.after)
    /\ r.stopped = Eligible(r.before, r.matching, r.owner, r.nonempty)
=============================================================================
