----------------------- MODULE WLoopCrashDropsDurable -----------------------
\* Reversed witness: a crash wipes the durable store instead of recovering what Flush made durable.
\* DurableMonotonic must reject it.
EXTENDS MCLoopPass

CrashWipe ==
    /\ crashes < MaxCrashes
    /\ crashes' = crashes + 1
    /\ stepIdx' = 1
    /\ failed' = FALSE /\ batching' = FALSE /\ holding' = FALSE /\ changed' = FALSE
    /\ disk' = EmptyStore /\ mem' = EmptyStore /\ batch' = <<>>
    /\ drvPhase' = "none" /\ drvTurn' = 0 /\ heldCmds' = <<>>
    /\ rcvd' = acked /\ applier' = [c \in Clients |-> 0]
    /\ linkSnap' = Snap(EmptyStore)
    /\ UNCHANGED <<acts, acked, drvSends, drvStops, sentSnap, fails>>

BrokenNext ==
    \/ \E c \in Clients, k \in Kinds : Act(c, k)
    \/ Tick(DefaultOrder)
    \/ Hold(DefaultOrder, TRUE)
    \/ \E c \in Clients : \E to \in 1..MaxActs : ApplyAct(DefaultOrder, c, to)
    \/ Applied(DefaultOrder)
    \/ FlushOk(DefaultOrder)
    \/ Release(DefaultOrder)
    \/ Publish(DefaultOrder)
    \/ CrashWipe

BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================
