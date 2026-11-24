------------------------ MODULE BusPeerFilterTrace -------------------------
EXTENDS BusPeerFilter, BusPeerFilterData, Sequences
VARIABLE row
traceVars == <<vars, row>>
TraceInit == Init /\ row = 1
Apply(r) == CASE r.op = "offer" -> Offer(r.id)
              [] r.op = "lookup" -> Read(r.answer, r.fresh)
              [] r.op = "decide" -> Decide("none")
TraceStep == IF row < Len(Trace) THEN /\ row' = row + 1 /\ Apply(Trace[row'])
             ELSE UNCHANGED traceVars
TraceSpec == TraceInit /\ [][TraceStep]_traceVars
Matches == /\ receipts = Trace[row].receipts /\ stored = Trace[row].stored
           /\ decision = Trace[row].decision
           /\ (history = "friend") = Trace[row].known
           /\ Trace[row].shown = (IF history = "friend" THEN stored ELSE {})
=============================================================================
