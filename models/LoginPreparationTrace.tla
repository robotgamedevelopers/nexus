------------------------ MODULE LoginPreparationTrace ------------------------
EXTENDS Sequences, LoginPreparation, LoginPreparationTraceData
VARIABLE cursor
allVars == <<cursor, vars>>
TraceInit == Init /\ cursor = 1
TraceNext == IF cursor < Len(Trace)
    THEN /\ cursor' = cursor + 1 /\ (IF Trace[cursor'].reset THEN /\ phase' = "waiting" /\ last' = Empty /\ from' = "waiting" ELSE Step(Trace[cursor'].input))
    ELSE UNCHANGED allVars
TraceSpec == TraceInit /\ [][TraceNext]_allVars
Matches == phase = Trace[cursor].phase
=============================================================================
