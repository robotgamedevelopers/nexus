--------------------------- MODULE WLoopTurnReuse ---------------------------
\* Reversed witness: recovery forgets the durable next turn number (resets next to 1 after cutting an open
\* turn), so the next prompt reuses turn 1. TurnsIncrease must reject it.
EXTENDS MCLoopPass

CrashResetTurn ==
    /\ crashes < MaxCrashes
    /\ LET rec == RecoverStore(disk)
           d1  == [rec.st EXCEPT !.next = 1]
       IN /\ crashes' = crashes + 1
          /\ stepIdx' = 1
          /\ failed' = FALSE /\ batching' = FALSE /\ holding' = FALSE /\ changed' = FALSE
          /\ disk' = d1 /\ mem' = d1 /\ batch' = <<>>
          /\ drvPhase' = rec.ph /\ drvTurn' = 0 /\ heldCmds' = <<>>
          /\ rcvd' = acked
          /\ applier' = [c \in Clients |-> d1.applied[c]]
          /\ linkSnap' = Snap(d1)
    /\ UNCHANGED <<acts, acked, drvSends, drvStops, sentSnap, fails>>

BrokenNext ==
    \/ \E c \in Clients, k \in Kinds : Act(c, k)
    \/ Tick(DefaultOrder)
    \/ Hold(DefaultOrder, TRUE)
    \/ DriverHello(DefaultOrder)
    \/ DriverDone(DefaultOrder)
    \/ \E c \in Clients : \E to \in 1..MaxActs : ApplyAct(DefaultOrder, c, to)
    \/ Applied(DefaultOrder)
    \/ FlushOk(DefaultOrder)
    \/ Release(DefaultOrder)
    \/ Publish(DefaultOrder)
    \/ CrashResetTurn

BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================
