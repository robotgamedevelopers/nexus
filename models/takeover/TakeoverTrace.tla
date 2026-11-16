---------------------------- MODULE TakeoverTrace ----------------------------
(***************************************************************************)
(* Trace validation for end-to-end takeover (`tests/server/takeover_walk`, *)
(* `models/takeover/Takeover.tla`): TLC checks that every trace emitted by *)
(* the real C++ `migrateOldLayout` + `Builds` + `Handover` +               *)
(* `SupervisorLoop` + `Store` engines on real POSIX files and OS locks is  *)
(* a valid behaviour of `Takeover.tla` (`Bug = "none"`), satisfying all    *)
(* cross-layer safety and action properties at every step.                 *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC, TakeoverTraceData

VARIABLES
    oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
    supLock, supPc, supDisk, explicitPick, srv, maxReader, sessions,
    wroteBreaking, handovers, cuts, supDeaths, turns, stops, l

Unbounded == 1000000

S == INSTANCE Takeover WITH
    AllowedBuilds <- {1, 2, 3},
    InitPlaced <- TInitPlaced,
    InitTested <- TInitTested,
    InitCur <- TInitCur,
    InitOldLayout <- TInitOldLayout,
    StartupBroken <- TStartupBroken,
    LateBroken <- TLateBroken,
    Format5Writers <- TFormat5Writers,
    MaxCrashesBeforeRollback <- TMaxCrashesBeforeRollback,
    MaxRestarts <- TMaxRestarts,
    MaxHandovers <- Unbounded,
    MaxCuts <- Unbounded,
    MaxSupDeaths <- Unbounded,
    MaxTurns <- Unbounded,
    MaxStops <- Unbounded,
    Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ oldLayout = r.oldLayout
    /\ migrated = r.migrated
    /\ placed = r.placed
    /\ tested = r.tested
    /\ cur = r.cur
    /\ prev = r.prev
    /\ hlock = r.hlock
    /\ hstate = r.hstate
    /\ supLock = r.supLock
    /\ supPc = r.supPc
    /\ supDisk = r.supDisk
    /\ explicitPick = r.explicitPick
    /\ srv = r.srv
    /\ maxReader = r.maxReader
    /\ sessions = r.sessions

TraceMatches == Matches(l)
TraceUnfinished == l < Len(Trace)

Step(r) ==
    CASE r.op = "old_stop" -> S!OldServerStop
      [] r.op = "migrate" -> S!Migrate
      [] r.op = "place" -> S!PlaceBuild(r.b, r.arg = 1)
      [] r.op = "init_cur" -> S!SetInitialCurrent(r.b)
      [] r.op = "ho_begin" -> S!HoBegin(r.b)
      [] r.op = "ho_resume" -> S!HoResumeAcquire
      [] r.op = "ho_drain" -> S!HoDrain
      [] r.op = "ho_start" -> S!HoStart
      [] r.op = "ho_rollback" -> S!HoRollback
      [] r.op = "ho_cut" -> S!HoCut
      [] r.op = "turn_open" -> S!TurnOpen
      [] r.op = "turn_done" -> S!TurnComplete
      [] r.op = "srv_write" -> S!SrvWriteFormat
      [] r.op = "srv_crash" -> S!SrvLateCrash
      [] r.op = "user_stop" -> S!UserStop
      [] r.op = "sup_start" -> S!SupStart
      [] r.op = "sup_check" -> S!SupCheck
      [] r.op = "sup_watch" -> S!SupWatch
      [] r.op = "sup_reap" -> S!SupReap
      [] r.op = "sup_rollback" -> S!SupRollback
      [] r.op = "sup_die" -> S!SupDie
      [] OTHER -> FALSE

traceVars == <<oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
               supLock, supPc, supDisk, explicitPick, srv, maxReader, sessions,
               wroteBreaking, handovers, cuts, supDeaths, turns, stops, l>>

TraceInit == l = 1 /\ S!Init /\ TraceMatches

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])
    /\ TraceMatches'

TraceDone == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ TraceDone]_traceVars

TypeOK == S!TypeOK
LiveOldUntouched == S!LiveOldUntouched
OldMigratedBeforeServe == S!OldMigratedBeforeServe
CurrentAlwaysTested == S!CurrentAlwaysTested
HandoverLockSerialized == S!HandoverLockSerialized
NeverIncompatibleCurrent == S!NeverIncompatibleCurrent
SessionsNeverLost == S!SessionsNeverLost
OpenTurnPreserved == S!OpenTurnPreserved
StopHonored == S!StopHonored
DrainNeverStops == S!DrainNeverStops
=============================================================================
