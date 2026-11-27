----------------------------- MODULE SessionGoal -----------------------------
EXTENDS Naturals
CONSTANT Interval
VARIABLE active, due, sends
vars == <<active, due, sends>>
Init == /\ active = FALSE /\ due = 0 /\ sends = 0
Set(now) == /\ active' = TRUE /\ due' = now + Interval /\ sends' = sends + 1
Off == /\ active' = FALSE /\ due' = 0 /\ UNCHANGED sends
Tick(now, pending) == IF active /\ now >= due
    THEN /\ UNCHANGED active /\ due' = now + Interval /\ sends' = sends + IF pending THEN 0 ELSE 1
    ELSE UNCHANGED vars
Step(event, now, pending) == CASE event = "set" -> Set(now)
    [] event = "off" -> Off
    [] event = "tick" -> Tick(now, pending)
    [] event = "restart" -> UNCHANGED vars
    [] OTHER -> UNCHANGED vars
=============================================================================
