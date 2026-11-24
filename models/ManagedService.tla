-------------------------- MODULE ManagedService --------------------------
EXTENDS Naturals, FiniteSets

\* One configured service name. A replacement nexus-server can start before
\* the previous server's helper has finished terminating the old process group.
\* A group remains live until its descendants have exited. Expire abstracts a
\* scheduled interval or keepalive retry deadline passing while the job is idle.
CONSTANT Fault
Owners == {1, 2}
Modes == {"off", "keep", "timer"}
VARIABLES mode, due, groups, claimed, timerRuns
vars == <<mode, due, groups, claimed, timerRuns>>

Init == /\ mode = [a \in Owners |-> "off"]
        /\ due = [a \in Owners |-> FALSE]
        /\ groups = {} /\ claimed = FALSE /\ timerRuns = 0

Configure(a, m) == /\ mode' = [mode EXCEPT ![a] = m]
                   /\ due' = IF m = mode[a] THEN due
                             ELSE [due EXCEPT ![a] = (m = "keep")]
                   /\ claimed' = IF Fault = "forget" /\ mode[a] = "off" THEN FALSE ELSE claimed
                   /\ UNCHANGED <<groups, timerRuns>>

\* A replacement can wake from its own deadline while the previous server's
\* durable timer claim still covers the interval. Only expiry of that claim
\* permits a second successful launch. timerRuns independently counts executions.
Expire(a, newInterval) ==
    /\ mode[a] # "off" /\ a \notin groups
    /\ due' = [due EXCEPT ![a] = TRUE]
    /\ claimed' = IF newInterval THEN FALSE ELSE claimed
    /\ timerRuns' = IF newInterval THEN 0 ELSE timerRuns
    /\ UNCHANGED <<mode, groups>>

Launch(a) == /\ mode[a] # "off" /\ (due[a] \/ Fault = "early")
             /\ a \notin groups /\ (groups = {} \/ Fault = "overlap")
             /\ (mode[a] # "timer" \/ ~claimed)
             /\ groups' = groups \cup {a}
             /\ due' = [due EXCEPT ![a] = FALSE]
             /\ claimed' = (claimed \/ mode[a] = "timer")
             /\ timerRuns' = IF mode[a] = "timer" THEN timerRuns + 1 ELSE timerRuns
             /\ UNCHANGED mode

Reap(a) == /\ a \in groups /\ groups' = groups \ {a}
           /\ UNCHANGED <<mode, due, claimed, timerRuns>>

Next == \E a \in Owners : \/ \E m \in Modes : Configure(a, m)
                           \/ \E newInterval \in BOOLEAN : Expire(a, newInterval)
                           \/ Launch(a) \/ Reap(a)
Spec == Init /\ [][Next]_vars

OneProcessGroup == Cardinality(groups) <= 1
OneRunPerInterval == timerRuns <= 1
OnlyWhenDue == [][\A a \in groups' \ groups : mode[a] # "off" /\ due[a]]_vars
=============================================================================
