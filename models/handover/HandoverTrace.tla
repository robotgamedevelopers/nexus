--------------------------- MODULE HandoverTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real handover code   *)
(* (src/server/handover.cpp, every step through Options::hook, over real   *)
(* `Builds` and `PosixFs` files and OS locks) is a behaviour of            *)
(* Handover.tla with `Bug = "none"`.                                       *)
(*                                                                         *)
(* tests/server/handover_walk.cpp writes HandoverTraceData.tla beside this *)
(* module: TTested, TBroken, TInitial, TStartSrv, and Trace, one record    *)
(* per step. As in PlacementTrace.tla, the trace is accepted when a        *)
(* behaviour reaches its last record (`TraceUnfinished` violated), and     *)
(* rejected when none does (`pass`). Every Handover invariant is checked   *)
(* at every step on the way.                                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC, HandoverTraceData

VARIABLES
    pc, verb, tgt, rtgt, rprev, didOwn, wf, lock, cur, prev,
    hf, hfd, hdir, hdirD, hpend, srv, ops, crashes, deaths, turns, l

Unbounded == 1000000

S == INSTANCE Handover WITH
    Actors <- {"a1", "a2"},
    Builds <- {1, 2, 3},
    Tested <- TTested,
    Broken <- TBroken,
    Initial <- TInitial,
    StartSrv <- TStartSrv,
    MaxOps <- Unbounded,
    MaxCrashes <- Unbounded,
    MaxDeaths <- Unbounded,
    MaxTurns <- Unbounded,
    Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ <<pc["a1"], pc["a2"]>> = r.pcs
    /\ lock = r.lock
    /\ cur = r.cur
    /\ prev = r.prev
    /\ S!HasState = r.hasState
    /\ (S!HasState =>
           /\ S!StateRec.kind = "rec"
           /\ S!StateRec.target = r.stTgt
           /\ S!StateRec.prev = r.stPrev
           /\ S!StateRec.stage = r.stStage)
    /\ Cardinality({n \in DOMAIN hdir : n.k = "t"}) = r.left
    /\ srv = r.srv

TraceMatches == Matches(l)
TraceUnfinished == l < Len(Trace)

Step(r) ==
    CASE r.op = "start"      -> S!Start(r.p, r.v, r.t)
      [] r.op = "lock"       -> S!LockTry(r.p)
      [] r.op = "giveup"     -> S!LockGiveUp(r.p)
      [] r.op = "settle"     -> S!Settle(r.p)
      [] r.op = "clean"      -> S!CleanOne(r.p)
      [] r.op = "inspect"    -> S!Inspect(r.p)
      [] r.op = "wswitch"    -> S!WriteState(r.p, "wswitch", "switching", "sswitch", "switch")
      [] r.op = "sswitch"    -> S!SyncState(r.p, "sswitch", "switch")
      [] r.op = "switch"     -> S!Switch(r.p)
      [] r.op = "wdrain"     -> S!WriteState(r.p, "wdrain", "draining", "sdrain", "drain")
      [] r.op = "sdrain"     -> S!SyncState(r.p, "sdrain", "drain")
      [] r.op = "drain"      -> S!DrainStep(r.p)
      [] r.op = "wstart"     -> S!WriteState(r.p, "wstart", "starting", "sstart", "start")
      [] r.op = "sstart"     -> S!SyncState(r.p, "sstart", "start")
      [] r.op = "start_srv"  -> S!StartStep(r.p)
      [] r.op = "wrollback"  -> S!WriteState(r.p, "wrollback", "rolling-back", "srollback", "rollback")
      [] r.op = "srollback"  -> S!SyncState(r.p, "srollback", "rollback")
      [] r.op = "rollback"   -> S!RollbackSwitch(r.p)
      [] r.op = "start_rb"   -> S!StartRollback(r.p)
      [] r.op = "clear"      -> S!ClearState(r.p)
      [] r.op = "sclear"     -> S!SyncClear(r.p)
      [] r.op = "unlock"     -> S!Unlock(r.p)
      [] r.op = "turn_open"  -> S!TurnOpen
      [] r.op = "turn_end"   -> S!TurnEnd
      [] r.op = "drain_late" -> S!DrainLate
      [] r.op = "srv_exit"   -> S!SrvDrainExit
      [] r.op = "die"        -> S!Die(r.p)
      [] r.op = "crash"      -> S!Crash
      [] OTHER               -> FALSE

traceVars == <<pc, verb, tgt, rtgt, rprev, didOwn, wf, lock, cur, prev,
               hf, hfd, hdir, hdirD, hpend, srv, ops, crashes, deaths, turns, l>>

TraceInit == l = 1 /\ S!Init /\ TraceMatches

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])
    /\ TraceMatches'

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == S!TypeOK
OneHandover == S!OneHandover
CurrentTested == S!CurrentTested
NeverServesBroken == S!NeverServesBroken
StateValid == S!StateValid
IdleCurrentWorks == S!IdleCurrentWorks
DrainCutsOnlyLate == S!DrainCutsOnlyLate
StopOnlyLegacy == S!StopOnlyLegacy

=============================================================================
