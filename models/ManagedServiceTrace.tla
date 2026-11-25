------------------------ MODULE ManagedServiceTrace ------------------------
EXTENDS ManagedService, ManagedServiceTraceData, Sequences

\* The functional fixture drives the production supervisor on a synthetic
\* deadline clock. Its OS lock and append-only start log are independent of
\* the supervisor. A blocked step before the end is a trace mismatch.
VARIABLES cursor, starts
allVars == <<vars, cursor, starts>>
Take(op) == CASE op = "timer" -> Configure(1, "timer")
              [] op = "off" -> Configure(1, "off")
              [] op = "due" -> Expire(1, TRUE)
              [] op = "launch" -> Launch(1)
              [] op = "exit" -> Reap(1)
              [] op = "observe" -> UNCHANGED vars
TraceInit == Init /\ cursor = 1 /\ starts = 0
TraceNext == IF cursor < Len(Trace) THEN /\ cursor' = cursor + 1 /\ Take(Trace[cursor'].op)
                                        /\ starts' = IF Trace[cursor'].op = "launch" THEN starts + 1 ELSE starts
             ELSE UNCHANGED allVars
TraceSpec == TraceInit /\ [][TraceNext]_allVars
Matches == Cardinality(groups) = Trace[cursor].groups /\ starts = Trace[cursor].starts
=============================================================================
