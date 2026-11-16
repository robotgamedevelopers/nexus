--------------------------- MODULE MigrationTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real migration,      *)
(* store-reader and rollback code (src/server/migration.cpp,               *)
(* src/store/store.cpp, src/server/builds.cpp) is a behaviour of           *)
(* Migration.tla with Bug = "none".                                        *)
(*                                                                         *)
(* tests/server/migration_walk.cpp writes MigrationTraceData.tla beside    *)
(* this module: the instance's initial sets (TActiveParts, TInitTop,       *)
(* TInitTopSock, TInitOldSrv, TInitDir) and Trace, one record per step:    *)
(*   op:  the step taken ("init", "start", "lock", "probe", "plan",        *)
(*        "plan_sync", "stage", "sync_stg", "rename", "sync_placed",       *)
(*        "clean_top", "sync_top", "finish", "unlock", "old_stop", "die",  *)
(*        "crash", "switch", "rollback", "srv_open", "srv_write",          *)
(*        "srv_compact", "srv_stop")                                       *)
(*   p:   actor ("a1", "a2", or "")                                        *)
(*   part: part name ("snap", "log", "drivers", "config", or "none")       *)
(*   b:   target build (1..3, or 0)                                        *)
(*   and after the step what the filesystem and processes show:            *)
(*     pcs, lock, oldSrv, top, topSock, dir, plan, refused,                *)
(*     curBuild, prevBuild, srvUp, maxReader, baseEntries, openFailed.     *)
(*                                                                         *)
(* As in PlacementTrace.tla, the trace is accepted when some behaviour     *)
(* reaches its last record (TraceUnfinished violated), and rejected when   *)
(* none does (TLC finishes with no error). Every Migration invariant holds *)
(* on every state along the way.                                           *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC, MigrationTraceData

VARIABLES
    pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
    dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur,
    refusedConflict, migratedOnce, curBuild, prevBuild, srvUp,
    maxReader, baseEntries, unknownChgRec, appendedTail, openFailed,
    runs, crashes, deaths, buildOps, l

Unbounded == 1000000

S == INSTANCE Migration WITH
    Actors <- {"a1", "a2"},
    ActiveParts <- TActiveParts,
    InitTop <- TInitTop,
    InitTopSock <- TInitTopSock,
    InitOldSrv <- TInitOldSrv,
    InitDir <- TInitDir,
    AllowedBuilds <- {1, 2, 3},
    MaxRuns <- Unbounded,
    MaxCrashes <- Unbounded,
    MaxDeaths <- Unbounded,
    MaxBuildOps <- Unbounded,
    Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ <<pc["a1"], pc["a2"]>> = r.pcs
    /\ migLock = r.lock
    /\ oldSrv = r.oldSrv
    /\ {p \in TActiveParts : topNow[p] # 0} = r.top
    /\ topSockNow = r.topSock
    /\ [p \in TActiveParts |-> dirNow[p]] = r.dir
    /\ planNow = r.plan
    /\ refusedConflict = r.refused
    /\ curBuild = r.curBuild
    /\ prevBuild = r.prevBuild
    /\ srvUp = r.srvUp
    /\ maxReader = r.maxReader
    /\ baseEntries = r.baseEntries
    /\ openFailed = r.openFailed

TraceMatches == Matches(l)
TraceUnfinished == l < Len(Trace)

Step(r) ==
    CASE r.op = "start" -> S!MStart(r.p)
      [] r.op = "lock" -> S!MLock(r.p)
      [] r.op = "probe" -> S!MProbe(r.p)
      [] r.op = "plan" -> S!MPlan(r.p)
      [] r.op = "plan_sync" -> S!MPlanSync(r.p)
      [] r.op = "stage" -> S!MStage(r.p, r.part)
      [] r.op = "sync_stg" -> S!MSyncStg(r.p)
      [] r.op = "rename" -> S!MRename(r.p)
      [] r.op = "sync_placed" -> S!MSyncPlaced(r.p)
      [] r.op = "clean_top" -> S!MCleanTop(r.p)
      [] r.op = "sync_top" -> S!MSyncTop(r.p)
      [] r.op = "finish" -> S!MFinish(r.p)
      [] r.op = "unlock" -> S!MUnlock(r.p)
      [] r.op = "old_stop" -> S!OldStop
      [] r.op = "die" -> S!Die(r.p)
      [] r.op = "crash" -> S!Crash
      [] r.op = "switch" -> S!SwitchBuild(r.b)
      [] r.op = "rollback" -> S!RollbackBuild
      [] r.op = "srv_open" -> S!SrvOpen
      [] r.op = "srv_write" -> S!SrvWrite
      [] r.op = "srv_compact" -> S!SrvCompact
      [] r.op = "srv_stop" -> S!SrvStop
      [] OTHER -> FALSE

traceVars == <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
               dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur,
               refusedConflict, migratedOnce, curBuild, prevBuild, srvUp,
               maxReader, baseEntries, unknownChgRec, appendedTail, openFailed,
               runs, crashes, deaths, buildOps, l>>

TraceInit == l = 1 /\ S!Init /\ TraceMatches

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])
    /\ TraceMatches'

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == S!TypeOK
LockHeld == S!LockHeld
LiveOldRefused == S!LiveOldRefused
NeverClobbersNewStore == S!NeverClobbersNewStore
OldDataNeverLost == S!OldDataNeverLost
MovesOnce == S!MovesOnce
CurAlwaysReadsStore == S!CurAlwaysReadsStore
RollbackReadable == S!RollbackReadable
=============================================================================
