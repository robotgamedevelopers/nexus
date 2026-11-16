---------------------------- MODULE WLoopNoTick ----------------------------
\* Reversed witness: Tick advances the pass without sending state or acknowledgements, so windows never
\* receive the server's state or acknowledgements. Converges must reject it.
EXTENDS MCLoopPass

SkipTick ==
    /\ ~failed /\ DefaultOrder[stepIdx] = "tick"
    /\ stepIdx' = NextIdx(DefaultOrder)
    /\ UNCHANGED <<failed, batching, holding, changed, acts, rcvd, applier, acked,
                   mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   linkSnap, sentSnap, crashes, fails>>

BrokenNext ==
    \/ \E c \in Clients, k \in Kinds : Act(c, k)
    \/ SkipTick
    \/ Hold(DefaultOrder, TRUE)
    \/ DriverHello(DefaultOrder)
    \/ DriverDone(DefaultOrder)
    \/ \E c \in Clients : \E to \in 1..MaxActs : RecvOnly(DefaultOrder, c, to)
    \/ \E c \in Clients : \E to \in 1..MaxActs : ApplyAct(DefaultOrder, c, to)
    \/ Applied(DefaultOrder)
    \/ FlushOk(DefaultOrder)
    \/ Release(DefaultOrder)
    \/ Publish(DefaultOrder)

BrokenFairness ==
    /\ WF_vars(SkipTick)
    /\ WF_vars(Hold(DefaultOrder, TRUE))
    /\ SF_vars(DriverHello(DefaultOrder))
    /\ SF_vars(DriverDone(DefaultOrder))
    /\ \A c \in Clients : \A to \in 1..MaxActs : SF_vars(RecvOnly(DefaultOrder, c, to))
    /\ \A c \in Clients : \A to \in 1..MaxActs : SF_vars(ApplyAct(DefaultOrder, c, to))
    /\ WF_vars(Applied(DefaultOrder))
    /\ WF_vars(FlushOk(DefaultOrder))
    /\ WF_vars(Release(DefaultOrder))
    /\ WF_vars(Publish(DefaultOrder))

BrokenSpec == Init /\ [][BrokenNext]_vars /\ BrokenFairness
=============================================================================
