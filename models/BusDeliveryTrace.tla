--------------------------- MODULE BusDeliveryTrace --------------------------
\* bus_delivery_trace.cpp pairs each live receipt and visible bubble count with
\* the same observations on a store reopened after dropping unsynced writes.
EXTENDS BusDelivery, BusDeliveryData
VARIABLE row
traceVars == <<vars, row>>
TraceInit == Init /\ row = 1
Apply(r) == CASE r.op = "receive" -> Receive(r.inbox, r.inbox \in mem.receipts)
              [] r.op = "clear" -> Clear(r.inbox)
              [] r.op = "flush" -> Flush
              [] r.op = "restart" -> Restart
TraceStep == IF row < Len(Trace) THEN /\ row' = row + 1 /\ Apply(Trace[row'])
             ELSE UNCHANGED traceVars
TraceSpec == TraceInit /\ [][TraceStep]_traceVars
Observed(s, i) == <<IF i \in s.receipts THEN 1 ELSE 0, s.visible[i]>>
Projection(s) == <<Observed(s, "ada"), Observed(s, "bo")>>
Matches == /\ Projection(mem) = Trace[row].mem /\ Projection(disk) = Trace[row].disk
=============================================================================
