-------------------------- MODULE WDrvStopLatched --------------------------
\* Reversed witness: a stop that finds its turn not open is kept (latched), and stops the next turn the query takes,
\* one the person never stopped. NoLatchedStop must reject it.
EXTENDS ClaudeDriver

VARIABLE latch      \* a stop kept for later

KeepingStop == \E n \in Turns : Stop(n) /\ latch' = (latch \/ n # open)

FiringTake ==
    /\ latch
    /\ phase # "ended"
    /\ stage = "pushed"
    /\ stage' = "taken"
    /\ stopping' = TRUE
    /\ intFor' = open
    /\ wrote' = <<>>
    /\ act' = [a |-> "step", n |-> 0]
    /\ latch' = FALSE
    /\ UNCHANGED <<phase, resume, open, textOpen, failure, pending, carry, inUse, last, secretVars, ghost>>

Others == NextWith(LAMBDA n : FALSE, Apply, Result, Refuse, NotFound) /\ UNCHANGED latch

BrokenSpec == Init /\ latch = FALSE /\ [][KeepingStop \/ FiringTake \/ Others]_<<vars, latch>>
=============================================================================
