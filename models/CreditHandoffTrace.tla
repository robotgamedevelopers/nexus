--------------------------- MODULE CreditHandoffTrace ---------------------------
\* tests/accounts/credit_handoff_trace.cpp records Hub::rotate, the effective
\* account and mode and the durable conversation (notices filtered out).
EXTENDS CreditHandoff, CreditHandoffData

VARIABLE line
traceVars == <<vars, line>>
Match(r) == /\ r.session = session /\ r.content = content /\ r.account = Current
            /\ r.pinned = pinned /\ r.eligible = eligible /\ r.stopped = stopped
TraceInit == Init /\ line = 1 /\ pinned = ExpectedPin /\ Current = "a" /\ eligible = {"a", "b", "c"}
TraceStep == /\ line < Len(Trace) /\ line' = line + 1 /\ Limit
TraceMatches == Match(Trace[line])
Done == /\ line = Len(Trace) /\ stopped /\ UNCHANGED traceVars
TraceSpec == TraceInit /\ [][TraceStep \/ Done]_traceVars
=============================================================================
