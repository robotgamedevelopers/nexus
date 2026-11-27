--------------------------- MODULE WorkGroupTrace ---------------------------
EXTENDS WorkGroup, WorkGroupTraceData, Sequences
VARIABLE cursor
allVars == <<cursor, opened>>
TraceInit == Init /\ cursor = 1
TraceNext == IF cursor < Len(Trace)
    THEN /\ cursor' = cursor + 1 /\ Step(Trace[cursor'].event)
    ELSE UNCHANGED allVars
TraceSpec == TraceInit /\ [][TraceNext]_allVars
Matches == opened = Trace[cursor].opened /\ opened = Trace[cursor].details
=============================================================================
