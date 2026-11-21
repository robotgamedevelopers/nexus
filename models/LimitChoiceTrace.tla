------------------------- MODULE LimitChoiceTrace -------------------------
EXTENDS LimitChoice, CreditHandoffData
VARIABLE line
all == <<vars, line>>
TraceInit == Init /\ line = 1
TraceStep == /\ line < Len(Trace) /\ line' = line + 1
             /\ Resolve(Trace[line'].choice, Trace[line'].fresh, TRUE)
Done == line = Len(Trace) /\ UNCHANGED all
TraceSpec == TraceInit /\ [][TraceStep \/ Done]_all
Matches == LET r == Trace[line] IN
           /\ account = r.account /\ model = r.model /\ work = r.work
           /\ pending = r.pending /\ commits = r.notices
           /\ r.request = IF pending THEN 4 ELSE 0
=============================================================================
