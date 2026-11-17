----------------------------- MODULE Migration -----------------------------
(***************************************************************************)
(* Layer 5 of replacing a running server (docs/roadmap.sexp,               *)
(* takeover-migration):                                                    *)
(*                                                                         *)
(*   1. Old-layout migration (~/.nexus -> ~/.nexus/server): a store,       *)
(*      drivers file and config from the layout before ~/.nexus/server     *)
(*      move once, under an OS lock, refusing while an old server still    *)
(*      serves on ~/.nexus/server.sock or holds ~/.nexus/store, refusing   *)
(*      rather than clobbering an already-populated ~/.nexus/server/store, *)
(*      and resuming from any crash or kill without losing, tearing or     *)
(*      duplicating any file or falsely refusing on resume.                *)
(*                                                                         *)
(*   2. Store format against rollback (docs/formats.md, docs/builds.md):   *)
(*      when build N+1 writes appended fields, unknown change kinds (even  *)
(*      a record whose only change is an unknown kind) and unknown entry   *)
(*      kinds with reader <= Writes(N), rolling back to build N opens and  *)
(*      replays the store with no session lost; and when a build raises    *)
(*      reader past Writes(N), switching back (or multi-hop downgrading)   *)
(*      checks the store's actual reader floor on disk: allowed when the   *)
(*      store holds nothing above Writes(N) (e.g. N+1 failed before        *)
(*      writing), refused when it does.                                    *)
(*                                                                         *)
(* Each action is one step of src/server/migration.cpp, src/store/store.cpp*)
(* and src/server/builds.cpp; its phase names are Options::hook's.         *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Actors,         \* processes running migrateOldLayout (e.g. {"a1", "a2"})
    ActiveParts,    \* subset of {"snap", "log", "drivers", "config"} active in this instance
    InitTop,        \* subset of ActiveParts initially present in ~/.nexus (value OldVal = 1)
    InitTopSock,    \* BOOLEAN: stale or live ~/.nexus/server.sock present at start
    InitOldSrv,     \* "off", "sock" (answers status), or "locked" (holds ~/.nexus/store)
    InitDir,        \* subset of ActiveParts already present in ~/.nexus/server (value NewVal = 2)
    AllowedBuilds,  \* subset of {1, 2, 3}: 1=(3/3), 2=(5/3 appended), 3=(5/5 breaking reader)
    MaxRuns,        \* migration attempts across actors
    MaxCrashes,     \* machine crashes (unsynced changes may revert or tear)
    MaxDeaths,      \* process kills (locks lost, volatile writes stay in OS cache)
    MaxBuildOps,    \* switches, rollbacks, opens, writes and compactions
    Bug             \* "none", or one reversed witness's break

AllParts == {"snap", "log", "drivers", "config"}
StoreParts == {"snap", "log"}

ASSUME ActiveParts \subseteq AllParts
ASSUME InitTop \subseteq ActiveParts /\ InitDir \subseteq ActiveParts
ASSUME InitOldSrv \in {"off", "sock", "locked"}
ASSUME AllowedBuilds \subseteq {1, 2, 3} /\ 1 \in AllowedBuilds

None == "none"
NoPart == "none"
OldVal == 1
NewVal == 2
TornVal == 3

\* Build 1: base build (writes 3, reader 3).
\* Build 2: compatible newer build (writes 5, reader 3: appended fields, unknown change/entry kinds).
\* Build 3: breaking-reader newer build (writes 5, reader 5).
Writes(b) == IF b = 1 THEN 3 ELSE 5
Reader(b) == IF b = 3 THEN 5 ELSE 3

VARIABLES
    pc,             \* [a \in Actors |-> phase]
    curPart,        \* [a \in Actors |-> part being staged/renamed, or NoPart]
    migLock,        \* holder of ~/.nexus/server migration lock, or None
    topNow,         \* [p \in ActiveParts |-> 0..2]: live ~/.nexus & ~/.nexus/store files
    topDur,         \* [p \in ActiveParts |-> 0..2]: synced ~/.nexus & ~/.nexus/store files
    topSockNow,     \* BOOLEAN: ~/.nexus/server.sock exists now
    topSockDur,     \* BOOLEAN: ~/.nexus/server.sock durable
    oldSrv,         \* "off", "sock", or "locked"
    dirNow,         \* [p \in ActiveParts |-> 0..3]: live ~/.nexus/server & store files
    dirDur,         \* [p \in ActiveParts |-> 0..3]: durable ~/.nexus/server & store files
    fileSynced,     \* [p \in ActiveParts |-> BOOLEAN]: bytes of dirNow[p] were fsynced before rename
    stgNow,         \* [p \in ActiveParts |-> 0..2]: live .mig-<p> staging file
    stgDur,         \* [p \in ActiveParts |-> 0..2]: synced .mig-<p> staging file
    planNow,        \* SUBSET ActiveParts: live ~/.nexus/server/.migrating (empty = absent)
    planDur,        \* SUBSET ActiveParts: synced ~/.nexus/server/.migrating
    refusedConflict,\* BOOLEAN: a run refused because both old and new held conflicting data
    migratedOnce,   \* BOOLEAN: ghost, set once migration finished with TopClean
    curBuild,       \* 1..3: builds/active/current
    prevBuild,      \* 0..3: builds/active/previous (0 = none)
    srvUp,          \* BOOLEAN: curBuild's server holds ~/.nexus/server/store open
    maxReader,      \* 0, 3, or 5: highest reader byte in ~/.nexus/server/store on disk
    baseEntries,    \* Nat: base sessions/entries durably in ~/.nexus/server/store
    unknownChgRec,  \* BOOLEAN: log holds a record whose only change is unknown to build 1
    appendedTail,   \* BOOLEAN: store holds appended fields / unknown entry kind from build >= 2
    openFailed,     \* BOOLEAN: Store::open failed on curBuild
    runs, crashes, deaths, buildOps

vars == <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
          dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur,
          refusedConflict, migratedOnce, curBuild, prevBuild, srvUp,
          maxReader, baseEntries, unknownChgRec, appendedTail, openFailed,
          runs, crashes, deaths, buildOps>>

HasTopStore(t) == \E p \in StoreParts \cap ActiveParts : t[p] # 0
HasDirStore(d) == \E p \in StoreParts \cap ActiveParts : d[p] # 0
TopClean == (\A p \in ActiveParts : topNow[p] = 0) /\ ~topSockNow
TopDurClean == (\A p \in ActiveParts : topDur[p] = 0) /\ ~topSockDur

\* Whether the initial state has a genuine conflict between ~/.nexus and ~/.nexus/server.
InitStoreConflict == (\E p \in StoreParts \cap ActiveParts : p \in InitTop) /\
                     (\E p \in StoreParts \cap ActiveParts : p \in InitDir)
InitFileConflict == \E p \in ({"drivers", "config"} \cap ActiveParts) : p \in InitTop /\ p \in InitDir
InitConflict == InitStoreConflict \/ InitFileConflict

\* Under Bug = "no_plan", .migrating is never written, so stage/rename work from live top -> dir directly.
EffectivePlan == IF planNow # {} THEN planNow
                 ELSE {p \in ActiveParts : topNow[p] # 0 /\ (Bug = "clobber_new" \/ dirNow[p] = 0)}

InitHasStore == (\E p \in StoreParts \cap InitTop : TRUE) \/ (\E p \in StoreParts \cap InitDir : TRUE)
InitBase == IF ~InitConflict /\ InitHasStore THEN 1 ELSE 0

Init ==
    /\ pc = [a \in Actors |-> "idle"]
    /\ curPart = [a \in Actors |-> NoPart]
    /\ migLock = None
    /\ topNow = [p \in ActiveParts |-> IF p \in InitTop THEN OldVal ELSE 0]
    /\ topDur = topNow
    /\ topSockNow = InitTopSock
    /\ topSockDur = InitTopSock
    /\ oldSrv = InitOldSrv
    /\ dirNow = [p \in ActiveParts |-> IF p \in InitDir THEN NewVal ELSE 0]
    /\ dirDur = dirNow
    /\ fileSynced = [p \in ActiveParts |-> p \in InitDir]
    /\ stgNow = [p \in ActiveParts |-> 0]
    /\ stgDur = [p \in ActiveParts |-> 0]
    /\ planNow = {}
    /\ planDur = {}
    /\ refusedConflict = FALSE
    /\ migratedOnce = (InitTop = {} /\ ~InitTopSock)
    /\ curBuild = 1
    /\ prevBuild = 0
    /\ srvUp = FALSE
    /\ maxReader = IF HasDirStore(dirNow) THEN 3 ELSE 0
    /\ baseEntries = IF HasDirStore(dirNow) THEN 1 ELSE 0
    /\ unknownChgRec = FALSE
    /\ appendedTail = FALSE
    /\ openFailed = FALSE
    /\ runs = 0
    /\ crashes = 0
    /\ deaths = 0
    /\ buildOps = 0

-----------------------------------------------------------------------------
(* Old-layout migration steps                                              *)
-----------------------------------------------------------------------------

MStart(a) ==
    /\ pc[a] = "idle"
    /\ IF TopClean /\ planNow = {}
       THEN /\ runs < MaxRuns
            /\ runs' = runs + 1
            /\ UNCHANGED <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                           dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur,
                           refusedConflict, migratedOnce, curBuild, prevBuild, srvUp,
                           maxReader, baseEntries, unknownChgRec, appendedTail, openFailed,
                           crashes, deaths, buildOps>>
       ELSE /\ (migLock = None /\ oldSrv = "off" /\ ~srvUp /\ (~InitConflict \/ ~refusedConflict)) \/ runs < MaxRuns
            /\ runs' = IF migLock = None /\ oldSrv = "off" /\ ~srvUp /\ (~InitConflict \/ ~refusedConflict) THEN runs ELSE runs + 1
            /\ pc' = [pc EXCEPT ![a] = "lock"]
            /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                           dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur,
                           refusedConflict, migratedOnce, curBuild, prevBuild, srvUp,
                           maxReader, baseEntries, unknownChgRec, appendedTail, openFailed,
                           crashes, deaths, buildOps>>

MLock(a) ==
    /\ pc[a] = "lock"
    /\ IF migLock = None \/ Bug = "no_lock"
       THEN /\ migLock' = IF Bug = "no_lock" THEN migLock ELSE a
            /\ pc' = [pc EXCEPT ![a] = "probe"]
       ELSE /\ pc' = [pc EXCEPT ![a] = "idle"]
            /\ UNCHANGED migLock
    /\ UNCHANGED <<curPart, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur,
                   refusedConflict, migratedOnce, curBuild, prevBuild, srvUp,
                   maxReader, baseEntries, unknownChgRec, appendedTail, openFailed,
                   runs, crashes, deaths, buildOps>>

MProbe(a) ==
    /\ pc[a] = "probe"
    /\ LET oldBusy == (Bug # "ignore_live_old") /\ (oldSrv # "off")
           newBusy == srvUp
       IN IF oldBusy \/ newBusy
          THEN /\ pc' = [pc EXCEPT ![a] = "idle"]
               /\ migLock' = IF migLock = a THEN None ELSE migLock
          ELSE /\ pc' = [pc EXCEPT ![a] = "plan"]
               /\ UNCHANGED migLock
    /\ UNCHANGED <<curPart, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur,
                   refusedConflict, migratedOnce, curBuild, prevBuild, srvUp,
                   maxReader, baseEntries, unknownChgRec, appendedTail, openFailed,
                   runs, crashes, deaths, buildOps>>

MPlan(a) ==
    /\ pc[a] = "plan"
    /\ stgNow' = [p \in ActiveParts |-> 0]
    /\ stgDur' = [p \in ActiveParts |-> 0]
    /\ IF TopClean /\ planNow = {}
       THEN /\ pc' = [pc EXCEPT ![a] = "unlock"]
            /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                           dirNow, dirDur, fileSynced, planNow, planDur, refusedConflict,
                           migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                           unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>
       ELSE IF planNow # {}
            THEN \* Resuming an interrupted migration: settle .migrating and directories before taking any step!
                 /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN
                                               (IF {p \in planNow : dirNow[p] = 0} # {} THEN "stage" ELSE "clean_top")
                                            ELSE "plan_sync"]
                 /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                                dirNow, dirDur, fileSynced, planNow, planDur, refusedConflict,
                                migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                                unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>
            ELSE \* Fresh migration: check conflicts before committing .migrating.
                 LET storeConflict == HasTopStore(topNow) /\ HasDirStore(dirNow)
                     fileConflict == \E p \in ({"drivers", "config"} \cap ActiveParts) :
                                         topNow[p] # 0 /\ dirNow[p] # 0 /\ topNow[p] # dirNow[p]
                     conflict == (Bug # "clobber_new") /\ (storeConflict \/ fileConflict)
                     toCopy == {p \in ActiveParts : topNow[p] # 0 /\ (Bug = "clobber_new" \/ dirNow[p] = 0)}
                 IN IF conflict
                    THEN /\ refusedConflict' = TRUE
                         /\ pc' = [pc EXCEPT ![a] = "unlock"]
                         /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                                        dirNow, dirDur, fileSynced, planNow, planDur,
                                        migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                                        unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>
                    ELSE IF toCopy = {}
                         THEN /\ pc' = [pc EXCEPT ![a] = "clean_top"]
                              /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                                             dirNow, dirDur, fileSynced, planNow, planDur, refusedConflict,
                                             migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                                             unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>
                         ELSE IF Bug = "no_plan"
                              THEN /\ pc' = [pc EXCEPT ![a] = "stage"]
                                   /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                                                  dirNow, dirDur, fileSynced, planNow, planDur, refusedConflict,
                                                  migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                                                  unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>
                              ELSE /\ planNow' = toCopy
                                   /\ pc' = [pc EXCEPT ![a] = "plan_sync"]
                                   /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                                                  dirNow, dirDur, fileSynced, planDur, refusedConflict,
                                                  migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                                                  unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

MPlanSync(a) ==
    /\ pc[a] = "plan_sync"
    /\ planDur' = planNow
    /\ dirDur' = dirNow
    /\ topDur' = topNow
    /\ topSockDur' = topSockNow
    /\ LET rem == {p \in planNow : dirNow[p] = 0}
       IN pc' = [pc EXCEPT ![a] = IF rem # {} THEN "stage" ELSE "clean_top"]
    /\ UNCHANGED <<curPart, migLock, topNow, topSockNow, oldSrv,
                   dirNow, fileSynced, stgNow, stgDur, planNow, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

MStage(a, p) ==
    /\ pc[a] = "stage"
    /\ p \in EffectivePlan
    /\ (Bug = "clobber_new" \/ dirNow[p] = 0)
    /\ topNow[p] # 0
    /\ stgNow' = [stgNow EXCEPT ![p] = topNow[p]]
    /\ stgDur' = [stgDur EXCEPT ![p] = 0]
    /\ curPart' = [curPart EXCEPT ![a] = p]
    /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN "rename" ELSE "sync_stg"]
    /\ UNCHANGED <<migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

MSyncStg(a) ==
    /\ pc[a] = "sync_stg"
    /\ LET p == curPart[a] IN
       /\ p \in ActiveParts
       /\ stgDur' = [stgDur EXCEPT ![p] = stgNow[p]]
       /\ pc' = [pc EXCEPT ![a] = "rename"]
    /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

MRename(a) ==
    /\ pc[a] = "rename"
    /\ LET p == curPart[a]
           synced == (stgDur[p] = stgNow[p] /\ stgNow[p] # 0)
           nextDir == [dirNow EXCEPT ![p] = stgNow[p]]
           rem == {q \in EffectivePlan : nextDir[q] = 0}
           gaveStore == p \in StoreParts /\ maxReader = 0
       IN /\ p \in ActiveParts
          /\ dirNow' = nextDir
          /\ fileSynced' = [fileSynced EXCEPT ![p] = synced]
          /\ stgNow' = [stgNow EXCEPT ![p] = 0]
          /\ stgDur' = [stgDur EXCEPT ![p] = 0]
          /\ curPart' = [curPart EXCEPT ![a] = NoPart]
          /\ maxReader' = IF gaveStore THEN 3 ELSE maxReader
          /\ baseEntries' = IF gaveStore THEN 1 ELSE baseEntries
          /\ pc' = [pc EXCEPT ![a] = IF rem # {} THEN "stage"
                                     ELSE IF Bug = "no_sync" THEN "clean_top"
                                     ELSE "sync_placed"]
    /\ UNCHANGED <<migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirDur, planNow, planDur, refusedConflict, migratedOnce,
                   curBuild, prevBuild, srvUp, unknownChgRec, appendedTail,
                   openFailed, runs, crashes, deaths, buildOps>>

MSyncPlaced(a) ==
    /\ pc[a] = "sync_placed"
    /\ dirDur' = dirNow
    /\ pc' = [pc EXCEPT ![a] = IF Bug = "leave_old" THEN "finish" ELSE "clean_top"]
    /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

MCleanTop(a) ==
    /\ pc[a] = "clean_top"
    /\ \/ \E p \in ActiveParts :
            /\ topNow[p] # 0
            /\ topNow' = [topNow EXCEPT ![p] = 0]
            /\ UNCHANGED topSockNow
            /\ pc' = [pc EXCEPT ![a] = IF (\A q \in ActiveParts \ {p} : topNow[q] = 0) /\ ~topSockNow
                                       THEN (IF Bug = "no_sync" THEN (IF planNow # {} THEN "finish" ELSE "unlock")
                                             ELSE "sync_top")
                                       ELSE "clean_top"]
       \/ /\ (\A p \in ActiveParts : topNow[p] = 0)
          /\ topSockNow
          /\ topSockNow' = FALSE
          /\ UNCHANGED topNow
          /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN (IF planNow # {} THEN "finish" ELSE "unlock")
                                     ELSE "sync_top"]
       \/ /\ TopClean
          /\ UNCHANGED <<topNow, topSockNow>>
          /\ pc' = [pc EXCEPT ![a] = IF planNow # {} THEN "finish" ELSE "unlock"]
    /\ UNCHANGED <<curPart, migLock, topDur, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

MSyncTop(a) ==
    /\ pc[a] = "sync_top"
    /\ topDur' = topNow
    /\ topSockDur' = topSockNow
    /\ pc' = [pc EXCEPT ![a] = IF planNow # {} THEN "finish" ELSE "unlock"]
    /\ UNCHANGED <<curPart, migLock, topNow, topSockNow, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

MFinish(a) ==
    /\ pc[a] = "finish"
    /\ planNow' = {}
    /\ planDur' = {}
    /\ dirDur' = dirNow
    /\ migratedOnce' = TRUE
    /\ pc' = [pc EXCEPT ![a] = "unlock"]
    /\ UNCHANGED <<curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, fileSynced, stgNow, stgDur, refusedConflict,
                   curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

MUnlock(a) ==
    /\ pc[a] = "unlock"
    /\ migLock' = IF migLock = a THEN None ELSE migLock
    /\ migratedOnce' = (migratedOnce \/ (TopClean /\ planNow = {} /\ ~refusedConflict))
    /\ pc' = [pc EXCEPT ![a] = "idle"]
    /\ UNCHANGED <<curPart, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

OldStop ==
    /\ oldSrv # "off"
    /\ oldSrv' = "off"
    /\ UNCHANGED <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths, buildOps>>

Die(a) ==
    /\ pc[a] # "idle"
    /\ deaths < MaxDeaths
    /\ deaths' = deaths + 1
    /\ pc' = [pc EXCEPT ![a] = "idle"]
    /\ curPart' = [curPart EXCEPT ![a] = NoPart]
    /\ migLock' = IF Bug = "stale_lock_file" THEN migLock
                  ELSE IF migLock = a THEN None ELSE migLock
    /\ UNCHANGED <<topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, buildOps>>

\* Machine crash: all processes end, locks release, and each unsynced directory/file change may revert or survive
\* (an unsynced renamed file whose staging was not synced may survive torn!).
Crash ==
    /\ crashes < MaxCrashes
    /\ crashes' = crashes + 1
    /\ pc' = [a \in Actors |-> "idle"]
    /\ curPart' = [a \in Actors |-> NoPart]
    /\ migLock' = IF Bug = "stale_lock_file" THEN migLock ELSE None
    /\ srvUp' = FALSE
    /\ stgNow' = stgDur
    /\ stgDur' = stgDur
    /\ \E keepPlan \in {planDur, planNow} :
         /\ planNow' = keepPlan
         /\ planDur' = keepPlan
    /\ \E keepTop \in SUBSET ActiveParts, keepSock \in {topSockDur, topSockNow} :
         /\ topNow' = [p \in ActiveParts |-> IF p \in keepTop THEN topNow[p] ELSE topDur[p]]
         /\ topDur' = topNow'
         /\ topSockNow' = keepSock
         /\ topSockDur' = keepSock
    /\ \E keepDir \in SUBSET ActiveParts :
         /\ dirNow' = [p \in ActiveParts |->
                         IF p \notin keepDir THEN dirDur[p]
                         ELSE IF dirNow[p] # 0 /\ ~fileSynced[p] THEN TornVal
                         ELSE dirNow[p]]
         /\ dirDur' = dirNow'
         /\ fileSynced' = [p \in ActiveParts |-> dirNow'[p] \in {OldVal, NewVal}]
    /\ UNCHANGED <<oldSrv, refusedConflict, migratedOnce, curBuild, prevBuild,
                   maxReader, baseEntries, unknownChgRec, appendedTail, openFailed,
                   runs, deaths, buildOps>>

-----------------------------------------------------------------------------
(* Build switches, rollbacks and store format checks                       *)
-----------------------------------------------------------------------------

ReadyForBuilds == TopClean /\ planNow = {} /\ migLock = None /\ ~InitConflict

SwitchBuild(b) ==
    /\ ReadyForBuilds
    /\ buildOps < MaxBuildOps
    /\ b \in AllowedBuilds \ {curBuild}
    \* Version rule (Placement): target reads what current declares its reader is.
    /\ Writes(b) >= Reader(curBuild)
    \* Layer 5 store rule: when downgrading writes (e.g. 5/3 -> 3/3), check what the store on disk actually needs.
    /\ IF Writes(b) < Writes(curBuild) /\ Bug # "multihop_no_store_check"
       THEN ~srvUp /\ maxReader <= Writes(b)
       ELSE TRUE
    /\ prevBuild' = curBuild
    /\ curBuild' = b
    /\ buildOps' = buildOps + 1
    /\ UNCHANGED <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, srvUp, maxReader, baseEntries, unknownChgRec, appendedTail,
                   openFailed, runs, crashes, deaths>>

RollbackBuild ==
    /\ ReadyForBuilds
    /\ buildOps < MaxBuildOps
    /\ prevBuild # 0
    \* Layer 5 rollback rule: if previous writes >= current writes and >= current reader, versions alone suffice;
    \* otherwise (rolling back to an older writer, including past a format bump that never wrote!), inspect the store
    \* under a shared lock (~srvUp) and allow iff maxReader <= Writes(prevBuild).
    /\ IF Writes(prevBuild) >= Writes(curBuild) /\ Writes(prevBuild) >= Reader(curBuild)
       THEN TRUE
       ELSE IF Bug = "blind_back"
            THEN ~srvUp
            ELSE ~srvUp /\ maxReader <= Writes(prevBuild)
    /\ curBuild' = prevBuild
    /\ prevBuild' = curBuild
    /\ buildOps' = buildOps + 1
    /\ UNCHANGED <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, srvUp, maxReader, baseEntries, unknownChgRec, appendedTail,
                   openFailed, runs, crashes, deaths>>

SrvOpen ==
    /\ migLock = None
    /\ ~srvUp
    /\ oldSrv = "off"
    /\ buildOps < MaxBuildOps
    \* A server starts only once old-layout migration has completed (unless Bug = "leave_old" left top behind).
    /\ planNow = {}
    /\ (TopClean \/ Bug = "leave_old")
    /\ ~InitConflict
    /\ LET badReader == maxReader > Writes(curBuild)
           badUnknown == (Bug = "refuse_unknown_change") /\ curBuild = 1 /\ unknownChgRec
       IN IF badReader \/ badUnknown
          THEN /\ openFailed' = TRUE
               /\ UNCHANGED srvUp
          ELSE /\ srvUp' = TRUE
               /\ UNCHANGED openFailed
    /\ buildOps' = buildOps + 1
    /\ UNCHANGED <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, maxReader, baseEntries,
                   unknownChgRec, appendedTail, runs, crashes, deaths>>

SrvWrite ==
    /\ srvUp
    /\ buildOps < MaxBuildOps
    /\ baseEntries' = baseEntries + 1
    /\ maxReader' = IF Reader(curBuild) > maxReader THEN Reader(curBuild) ELSE maxReader
    /\ unknownChgRec' = (unknownChgRec \/ curBuild = 2)
    /\ appendedTail' = (appendedTail \/ curBuild >= 2)
    /\ buildOps' = buildOps + 1
    /\ UNCHANGED <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, openFailed, runs, crashes, deaths>>

SrvCompact ==
    /\ srvUp
    /\ buildOps < MaxBuildOps
    /\ LET sv == IF "snap" \in ActiveParts /\ dirNow["snap"] = 0
                 THEN (IF "log" \in ActiveParts /\ dirNow["log"] # 0 THEN dirNow["log"] ELSE NewVal)
                 ELSE IF "snap" \in ActiveParts THEN dirNow["snap"] ELSE 0
       IN /\ dirNow' = IF "snap" \in ActiveParts THEN [dirNow EXCEPT !["snap"] = sv] ELSE dirNow
          /\ dirDur' = IF "snap" \in ActiveParts THEN [dirDur EXCEPT !["snap"] = sv] ELSE dirDur
          /\ fileSynced' = IF "snap" \in ActiveParts THEN [fileSynced EXCEPT !["snap"] = TRUE] ELSE fileSynced
    /\ maxReader' = Reader(curBuild)
    /\ unknownChgRec' = FALSE
    /\ appendedTail' = (curBuild >= 2)
    /\ buildOps' = buildOps + 1
    /\ UNCHANGED <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, srvUp, baseEntries, openFailed,
                   runs, crashes, deaths>>

SrvStop ==
    /\ srvUp
    /\ buildOps < MaxBuildOps
    /\ srvUp' = FALSE
    /\ buildOps' = buildOps + 1
    /\ UNCHANGED <<pc, curPart, migLock, topNow, topDur, topSockNow, topSockDur, oldSrv,
                   dirNow, dirDur, fileSynced, stgNow, stgDur, planNow, planDur, refusedConflict,
                   migratedOnce, curBuild, prevBuild, maxReader, baseEntries,
                   unknownChgRec, appendedTail, openFailed, runs, crashes, deaths>>

MigStep(a) ==
    \/ MLock(a)
    \/ MProbe(a)
    \/ MPlan(a)
    \/ MPlanSync(a)
    \/ \E p \in ActiveParts : MStage(a, p)
    \/ MSyncStg(a)
    \/ MRename(a)
    \/ MSyncPlaced(a)
    \/ MCleanTop(a)
    \/ MSyncTop(a)
    \/ MFinish(a)
    \/ MUnlock(a)

MigratedClean ==
    InitConflict \/
    (TopClean /\ TopDurClean /\ planNow = {} /\ planDur = {} /\
     (\A p \in InitTop : dirNow[p] = OldVal /\ dirDur[p] = OldVal))

Finished ==
    /\ \A a \in Actors : pc[a] = "idle"
    /\ \/ (MigratedClean /\ (InitConflict => refusedConflict))
       \/ (Bug = "stale_lock_file" /\ migLock # None /\ runs = MaxRuns)
    /\ UNCHANGED vars

Next ==
    \/ \E a \in Actors : MStart(a) \/ MigStep(a) \/ Die(a)
    \/ OldStop
    \/ Crash
    \/ \E b \in AllowedBuilds : SwitchBuild(b)
    \/ RollbackBuild
    \/ SrvOpen
    \/ SrvWrite
    \/ SrvCompact
    \/ SrvStop
    \/ Finished

Spec == Init /\ [][Next]_vars

\* Fairness for CutAnywhereResumes: once an old server stops and an actor can start/continue migration, it finishes.
Fairness ==
    /\ WF_vars(OldStop)
    /\ \A a \in Actors : WF_vars(MigStep(a)) /\ WF_vars(MStart(a))

Live == Spec /\ Fairness

-----------------------------------------------------------------------------
(* Invariants and Liveness                                                 *)
-----------------------------------------------------------------------------

Pcs == {"idle", "lock", "probe", "plan", "plan_sync", "stage", "sync_stg",
        "rename", "sync_placed", "clean_top", "sync_top", "finish", "unlock"}

TypeOK ==
    /\ pc \in [Actors -> Pcs]
    /\ curPart \in [Actors -> ActiveParts \cup {NoPart}]
    /\ migLock \in Actors \cup {None}
    /\ topNow \in [ActiveParts -> 0..2]
    /\ topDur \in [ActiveParts -> 0..2]
    /\ topSockNow \in BOOLEAN /\ topSockDur \in BOOLEAN
    /\ oldSrv \in {"off", "sock", "locked"}
    /\ dirNow \in [ActiveParts -> 0..3]
    /\ dirDur \in [ActiveParts -> 0..3]
    /\ fileSynced \in [ActiveParts -> BOOLEAN]
    /\ stgNow \in [ActiveParts -> 0..2]
    /\ stgDur \in [ActiveParts -> 0..2]
    /\ planNow \in SUBSET ActiveParts
    /\ planDur \in SUBSET ActiveParts
    /\ curBuild \in AllowedBuilds
    /\ prevBuild \in {0} \cup AllowedBuilds
    /\ maxReader \in {0, 3, 5}
    /\ baseEntries \in Nat
    /\ openFailed \in BOOLEAN

\* Every migration step past `lock` is taken only by the holder of migLock, and at most one actor is past `lock`.
LockHeld ==
    /\ \A a \in Actors : pc[a] \notin {"idle", "lock"} => migLock = a
    /\ \A a, b \in Actors : (pc[a] \notin {"idle", "lock"} /\ pc[b] \notin {"idle", "lock"}) => a = b

\* While an old server still serves on ~/.nexus/server.sock or holds ~/.nexus/store, nothing in top or dir is touched.
LiveOldRefused ==
    oldSrv # "off" =>
        /\ topNow = [p \in ActiveParts |-> IF p \in InitTop THEN OldVal ELSE 0]
        /\ topSockNow = InitTopSock
        /\ dirNow = [p \in ActiveParts |-> IF p \in InitDir THEN NewVal ELSE 0]
        /\ planNow = {}

\* When ~/.nexus/server already held conflicting data, neither ~/.nexus/server nor ~/.nexus is overwritten or removed.
NeverClobbersNewStore ==
    InitConflict =>
        /\ topNow = [p \in ActiveParts |-> IF p \in InitTop THEN OldVal ELSE 0]
        /\ dirNow = [p \in ActiveParts |-> IF p \in InitDir THEN NewVal ELSE 0]
        /\ planNow = {}

\* No file of an unconflicted old layout is ever torn, lost from both homes, or blocked by a false conflict after crash.
OldDataNeverLost ==
    ~InitConflict =>
        /\ ~refusedConflict
        /\ \A p \in ActiveParts : dirNow[p] # TornVal /\ dirDur[p] # TornVal
        /\ \A p \in InitTop :
             (dirDur[p] = OldVal) \/ (topDur[p] = OldVal)

\* Migration moves once: a server never opens with old layout left behind, and once migrated top stays clean forever.
MovesOnce ==
    /\ srvUp => (TopClean /\ planNow = {})
    /\ migratedOnce => (TopClean /\ planNow = {})

\* The current build can always read every record and snapshot in ~/.nexus/server/store.
CurAlwaysReadsStore ==
    (~InitConflict) => maxReader <= Writes(curBuild)

\* Rolling back from a compatible newer build (appended fields, unknown change-only record, unknown entry kind) opens
\* cleanly and loses no base session or entry.
RollbackReadable ==
    /\ ~openFailed
    /\ (ReadyForBuilds /\ ~InitConflict) => baseEntries >= InitBase

\* Under fairness (with enough MaxRuns past crashes/deaths), migration completes cleanly with every old part durable.
CutAnywhereResumes == <>MigratedClean

=============================================================================
