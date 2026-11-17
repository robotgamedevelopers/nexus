------------------------- MODULE SupervisionTrace -------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real supervision     *)
(* engine (src/server/supervision.cpp, over real `Builds` and `PosixFs`    *)
(* files and OS locks) is a behaviour of Supervision.tla (`Bug = "none"`). *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC, SupervisionTraceData

VARIABLES
    pc, sbuild, scrashes, sshunned, supLock, hlock, hTarget, explicitPick,
    cur, prev, disk, child, stopped, halted, rbHistory, races, supDeaths, handovers, stops, l

Unbounded == 1000000

S == INSTANCE Supervision WITH
    SupActors <- {"s1", "s2"},
    Builds <- {1, 2, 3},
    Tested <- TTested,
    StartupBroken <- TStartupBroken,
    LateBroken <- TLateBroken,
    Initial <- TInitial,
    MaxCrashesBeforeRollback <- 2,
    MaxRestarts <- 3,
    MaxLockRaces <- Unbounded,
    MaxSupDeaths <- Unbounded,
    MaxHandovers <- Unbounded,
    MaxStops <- Unbounded,
    Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ <<pc["s1"], pc["s2"]>> = r.pcs
    /\ supLock = r.supLock
    /\ hlock = r.hlock
    /\ cur = r.cur
    /\ prev = r.prev
    /\ disk = r.disk
    /\ child = r.child
    /\ stopped = r.stopped
    /\ halted = r.halted

TraceMatches == Matches(l)
TraceUnfinished == l < Len(Trace)

Step(r) ==
    CASE r.op = "ensure"       -> S!EnsureSup(r.p)
      [] r.op = "race_start"   -> S!RaceSupStart(r.p)
      [] r.op = "lock"         -> S!SupLockTry(r.p)
      [] r.op = "giveup"       -> S!SupLockGiveUp(r.p)
      [] r.op = "load"         -> S!SupLoad(r.p)
      [] r.op = "check"        -> S!SupCheck(r.p)
      [] r.op = "spawn"        -> S!SupSpawn(r.p)
      [] r.op = "watch"        -> S!SupWatch(r.p)
      [] r.op = "reap"         -> S!SupReap(r.p)
      [] r.op = "backoff"      -> S!SupBackoff(r.p)
      [] r.op = "rb_lock"      -> S!SupRbLock(r.p)
      [] r.op = "rollback"     -> S!SupRollback(r.p)
      [] r.op = "rb_unlock"    -> S!SupRbUnlock(r.p)
      [] r.op = "unlock"       -> S!SupUnlock(r.p)
      [] r.op = "stable"       -> S!ChildStable(r.p)
      [] r.op = "sup_die"      -> S!SupDie(r.p)
      [] r.op = "late_crash"   -> S!ChildLateCrash
      [] r.op = "user_stop"    -> S!UserStop
      [] r.op = "ho_begin"     -> S!HandoverBegin(r.t)
      [] r.op = "ho_finish"    -> S!HandoverFinish
      [] OTHER                 -> FALSE

traceVars == <<pc, sbuild, scrashes, sshunned, supLock, hlock, hTarget, explicitPick,
               cur, prev, disk, child, stopped, halted, rbHistory, races, supDeaths, handovers, stops, l>>

TraceInit == l = 1 /\ S!Init /\ TraceMatches

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])
    /\ TraceMatches'

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == S!TypeOK
OneSupervisor == S!OneSupervisor
HandoverSerialized == S!HandoverSerialized
CurrentTested == S!CurrentTested
RestartsBounded == S!RestartsBounded
NeverRollsBackToShunned == S!NeverRollsBackToShunned
StopStaysStopped == S!StopStaysStopped
DrainRestartsCurrent == S!DrainRestartsCurrent

=============================================================================
