---------------------------- MODULE Supervision ----------------------------
(***************************************************************************)
(* Supervision: keeping nexus-server running across boots, drain handovers *)
(* (exit 75), intentional stops (exit 0), and post-start build failures    *)
(* (src/server/supervision.h, docs/builds.md; docs/roadmap.sexp            *)
(* `takeover-supervision`, layer 4 of 6).                                  *)
(*                                                                         *)
(* Layer 1 (StoreLock.tla) keeps at most one server open on the store;     *)
(* Layer 2 (Placement.tla) keeps placed builds immutable and switches      *)
(* (current, previous) atomically; Layer 3 (Handover.tla) drains the old   *)
(* server and checks that a newly switched build reaches SERVING.          *)
(* Layer 4 sits above them:                                                *)
(*   - `launchd` (macOS LaunchAgent) and `systemd` (Linux/WSL2 user unit)  *)
(*     run `<server dir>/current/bin/nexus-server supervise --dir <dir>`.  *)
(*   - One supervisor runs per server directory under an exclusive OS      *)
(*     flock on `<server dir>/supervise`.                                  *)
(*   - Durable state in `<server dir>/supervise/state` tracks consecutive  *)
(*     crashes (`crashes`), builds already rolled back from (`shunned`),   *)
(*     and whether supervision halted (`halted`), surviving supervisor     *)
(*     restarts so a dual-broken pair never ping-pongs.                    *)
(*   - Child exit 0 (`nexus-server stop`): supervisor stops cleanly (0),   *)
(*     and `KeepAlive.SuccessfulExit=false` / `Restart=on-failure` leaves  *)
(*     the service stopped.                                                *)
(*   - Child exit 75 (`kDrainExit`): supervisor resets `crashes` and       *)
(*     immediately starts the active `current` build once `handover/` is   *)
(*     unlocked.                                                           *)
(*   - Child crash (`exit != 0, 75`, including post-SERVING crashes):      *)
(*     increments `crashes`; at `MaxCrashesBeforeRollback`, acquires       *)
(*     `handover/`'s exclusive lock, shuns `current`, switches             *)
(*     `builds/active` back to `previous` (if non-zero and not shunned),   *)
(*     and starts `previous`. If no unshunned `previous` exists, backs off *)
(*     and halts (`kExitHalted = 78`) at `MaxRestarts`.                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    SupActors,                \* supervisor processes, e.g. {"s1", "s2"}
    Builds,                   \* placed builds, e.g. {1, 2, 3}
    Tested,                   \* subset of Builds with passing test evidence
    StartupBroken,            \* subset of Builds that crash before SERVING
    LateBroken,               \* subset of Builds that reach SERVING, then crash later
    Initial,                  \* initial active build
    MaxCrashesBeforeRollback, \* crashes on `current` before rolling back to `previous`
    MaxRestarts,              \* crashes without an eligible rollback before halting
    MaxLockRaces,             \* concurrent second-supervisor start attempts
    MaxSupDeaths,             \* crashes of the supervisor process itself
    MaxHandovers,             \* external `handover <build>` transitions
    MaxStops,                 \* intentional `nexus-server stop` requests
    Bug                       \* "none", or one reversed witness break

None == "none"
HandoverHolder == "ho"

ExitStop == 0
ExitDrain == 75
ExitCrash == 1

ASSUME Builds \subseteq 1..3 /\ Tested \subseteq Builds
ASSUME StartupBroken \subseteq Builds /\ LateBroken \subseteq Builds
ASSUME Initial \in Tested
ASSUME MaxCrashesBeforeRollback >= 1 /\ MaxRestarts >= MaxCrashesBeforeRollback
ASSUME Bug \in {"none", "no_sup_lock", "no_handover_lock", "drain_stops", "stop_restarts",
                "no_post_start_rollback", "ping_pong_rollback", "no_halt_bound",
                "in_memory_sup_state", "serve_untested"}

DownChild == [b |-> 0, state |-> "down", code |-> 0]
EmptyDisk == [build |-> 0, crashes |-> 0, shunned |-> {}, halted |-> FALSE, stopped |-> FALSE]

VARIABLES
    pc,          \* each supervisor actor's next step (matches Options::hook in C++)
    sbuild,      \* each supervisor's tracked build
    scrashes,    \* each supervisor's consecutive crash count for `sbuild`
    sshunned,    \* each supervisor's shunned build set
    supLock,     \* holder of `<dir>/supervise` flock, or None
    hlock,       \* holder of `<dir>/handover` flock: a supervisor, HandoverHolder, or None
    hTarget,     \* target build of an in-flight external handover (0 when none)
    explicitPick,\* build most recently chosen by an explicit handover (clears shun for that build once)
    cur,         \* `builds/active` current build
    prev,        \* `builds/active` previous build (0 when none)
    disk,        \* synced `<dir>/supervise/state`: [build, crashes, shunned, halted]
    child,       \* supervised server process: [b, state \in {"down","serving","exited"}, code]
    stopped,     \* TRUE after an intentional `stop` until a new start/handover
    halted,      \* TRUE when supervision halted after exhausting rollback and restart bounds
    rbHistory,   \* ghost: builds rolled back from since the last explicit handover to them
    races, supDeaths, handovers, stops

vars == <<pc, sbuild, scrashes, sshunned, supLock, hlock, hTarget, explicitPick,
          cur, prev, disk, child, stopped, halted, rbHistory, races, supDeaths, handovers, stops>>

SupLocals == <<sbuild, scrashes, sshunned>>
Counts == <<races, supDeaths, handovers, stops>>

Init ==
    /\ pc = [s \in SupActors |-> "idle"]
    /\ sbuild = [s \in SupActors |-> 0]
    /\ scrashes = [s \in SupActors |-> 0]
    /\ sshunned = [s \in SupActors |-> {}]
    /\ supLock = None
    /\ hlock = None
    /\ hTarget = 0
    /\ explicitPick = 0
    /\ cur = Initial
    /\ prev = 0
    /\ disk = EmptyDisk
    /\ child = DownChild
    /\ stopped = FALSE
    /\ halted = FALSE
    /\ rbHistory = {}
    /\ races = 0
    /\ supDeaths = 0
    /\ handovers = 0
    /\ stops = 0

ResetSup(s) ==
    /\ sbuild' = [sbuild EXCEPT ![s] = 0]
    /\ scrashes' = [scrashes EXCEPT ![s] = 0]
    /\ sshunned' = [sshunned EXCEPT ![s] = {}]

PersistDisk(b, c, sh, hlt, stp) ==
    IF Bug = "in_memory_sup_state"
    THEN UNCHANGED disk
    ELSE disk' = [build |-> b, crashes |-> c, shunned |-> sh, halted |-> hlt, stopped |-> stp]

\* Service manager starts a supervisor when none is running (and service is neither stopped nor halted).
EnsureSup(s) ==
    /\ \A x \in SupActors : pc[x] = "idle"
    /\ ~stopped
    /\ ~halted
    /\ pc' = [pc EXCEPT ![s] = "lock"]
    /\ ResetSup(s)
    /\ UNCHANGED <<supLock, hlock, hTarget, explicitPick, cur, prev, disk, child, stopped, halted, rbHistory, Counts>>

\* Concurrent second `supervise` invocation racing on `supLock`.
RaceSupStart(s) ==
    /\ pc[s] = "idle"
    /\ \E x \in SupActors : pc[x] # "idle"
    /\ races < MaxLockRaces
    /\ ~stopped
    /\ ~halted
    /\ pc' = [pc EXCEPT ![s] = "lock"]
    /\ ResetSup(s)
    /\ races' = races + 1
    /\ UNCHANGED <<supLock, hlock, hTarget, explicitPick, cur, prev, disk, child, stopped, halted, rbHistory, supDeaths, handovers, stops>>

SupLockTry(s) ==
    /\ pc[s] = "lock"
    /\ IF Bug = "no_sup_lock"
       THEN /\ pc' = [pc EXCEPT ![s] = "load"]
            /\ UNCHANGED supLock
       ELSE IF supLock = None
       THEN /\ supLock' = s
            /\ pc' = [pc EXCEPT ![s] = "load"]
       ELSE UNCHANGED <<pc, supLock>>
    /\ UNCHANGED <<SupLocals, hlock, hTarget, explicitPick, cur, prev, disk, child, stopped, halted, rbHistory, Counts>>

SupLockGiveUp(s) ==
    /\ pc[s] = "lock" /\ supLock # None /\ supLock # s
    /\ pc' = [pc EXCEPT ![s] = "idle"]
    /\ ResetSup(s)
    /\ UNCHANGED <<supLock, hlock, hTarget, explicitPick, cur, prev, disk, child, stopped, halted, rbHistory, Counts>>

\* Load `<dir>/supervise/state` under the supervise lock.
SupLoad(s) ==
    /\ pc[s] = "load"
    /\ sbuild' = [sbuild EXCEPT ![s] = disk.build]
    /\ scrashes' = [scrashes EXCEPT ![s] = disk.crashes]
    /\ sshunned' = [sshunned EXCEPT ![s] = disk.shunned]
    /\ IF disk.stopped /\ Bug # "stop_restarts"
       THEN /\ stopped' = TRUE
            /\ UNCHANGED halted
            /\ pc' = [pc EXCEPT ![s] = "unlock"]
       ELSE IF disk.halted /\ disk.build = cur /\ explicitPick # cur /\ Bug # "no_halt_bound"
       THEN /\ halted' = TRUE
            /\ UNCHANGED stopped
            /\ pc' = [pc EXCEPT ![s] = "unlock"]
       ELSE /\ UNCHANGED <<stopped, halted>>
            /\ pc' = [pc EXCEPT ![s] = "check"]
    /\ UNCHANGED <<supLock, hlock, hTarget, explicitPick, cur, prev, disk, child, rbHistory, Counts>>

\* Check `handover/` lock and `builds/active` before spawning `cur`.
SupCheck(s) ==
    /\ pc[s] = "check"
    /\ IF hlock # None /\ hlock # s
       THEN UNCHANGED <<pc, SupLocals, explicitPick, disk, stopped, halted>>
       ELSE IF disk.stopped /\ Bug # "stop_restarts"
       THEN /\ stopped' = TRUE
            /\ pc' = [pc EXCEPT ![s] = "unlock"]
            /\ UNCHANGED <<SupLocals, explicitPick, disk, halted>>
       ELSE IF disk.halted /\ disk.build = cur /\ explicitPick # cur /\ Bug # "no_halt_bound"
       THEN /\ halted' = TRUE
            /\ pc' = [pc EXCEPT ![s] = "unlock"]
            /\ UNCHANGED <<SupLocals, explicitPick, disk, stopped>>
       ELSE IF cur \notin Tested /\ Bug # "serve_untested"
       THEN /\ halted' = TRUE
            /\ PersistDisk(cur, scrashes[s], sshunned[s], TRUE, FALSE)
            /\ pc' = [pc EXCEPT ![s] = "unlock"]
            /\ UNCHANGED <<SupLocals, explicitPick, stopped>>
       ELSE LET newShun == IF explicitPick = cur THEN sshunned[s] \ {cur} ELSE sshunned[s]
                newCrashes == IF sbuild[s] = cur /\ explicitPick # cur THEN scrashes[s] ELSE 0
                eligRb == (Bug # "no_post_start_rollback")
                          /\ newCrashes >= MaxCrashesBeforeRollback
                          /\ prev # 0
                          /\ (Bug = "ping_pong_rollback" \/ prev \notin newShun)
                mustHalt == (Bug # "no_halt_bound") /\ newCrashes >= MaxRestarts
            IN /\ sbuild' = [sbuild EXCEPT ![s] = cur]
               /\ scrashes' = [scrashes EXCEPT ![s] = newCrashes]
               /\ sshunned' = [sshunned EXCEPT ![s] = newShun]
               /\ explicitPick' = IF explicitPick = cur THEN 0 ELSE explicitPick
               /\ UNCHANGED stopped
               /\ IF child.state = "serving" /\ child.b = cur
                  THEN /\ PersistDisk(cur, newCrashes, newShun, FALSE, FALSE)
                       /\ halted' = FALSE
                       /\ pc' = [pc EXCEPT ![s] = "watch"]
                  ELSE IF child.state = "exited"
                  THEN /\ PersistDisk(cur, newCrashes, newShun, FALSE, FALSE)
                       /\ halted' = FALSE
                       /\ pc' = [pc EXCEPT ![s] = "reap"]
                  ELSE IF eligRb
                  THEN /\ PersistDisk(cur, newCrashes, newShun, FALSE, FALSE)
                       /\ halted' = FALSE
                       /\ pc' = [pc EXCEPT ![s] = "rb_lock"]
                  ELSE IF mustHalt
                  THEN /\ PersistDisk(cur, newCrashes, newShun, TRUE, FALSE)
                       /\ halted' = TRUE
                       /\ pc' = [pc EXCEPT ![s] = "unlock"]
                  ELSE /\ PersistDisk(cur, newCrashes, newShun, FALSE, FALSE)
                       /\ halted' = FALSE
                       /\ pc' = [pc EXCEPT ![s] = "spawn"]
    /\ UNCHANGED <<supLock, hlock, hTarget, cur, prev, child, rbHistory, Counts>>

\* Spawn `<dir>/current/bin/nexus-server serve --dir <dir>`.
\* If `handover` locked or switched `cur` since `check`, re-check before spawning so `sbuild` matches `cur`.
SupSpawn(s) ==
    /\ pc[s] = "spawn"
    /\ child.state = "down"
    /\ IF (hlock # None /\ hlock # s) \/ cur # sbuild[s] \/ (disk.stopped /\ Bug # "stop_restarts")
       THEN /\ pc' = [pc EXCEPT ![s] = "check"]
            /\ UNCHANGED child
       ELSE IF cur \in StartupBroken
       THEN /\ child' = [b |-> cur, state |-> "exited", code |-> ExitCrash]
            /\ pc' = [pc EXCEPT ![s] = "reap"]
       ELSE /\ child' = [b |-> cur, state |-> "serving", code |-> 0]
            /\ pc' = [pc EXCEPT ![s] = "watch"]
    /\ UNCHANGED <<SupLocals, supLock, hlock, hTarget, explicitPick, cur, prev, disk, stopped, halted, rbHistory, Counts>>

\* Watch running server: transitions to `reap` when the child exits, or drains it if settled `cur` moved away from
\* `child.b` (e.g. `builds current <hash>` or a handover that finished while `child` was down).
SupWatch(s) ==
    /\ pc[s] = "watch"
    /\ \/ /\ child.state = "exited"
          /\ pc' = [pc EXCEPT ![s] = "reap"]
          /\ UNCHANGED child
       \/ /\ hlock = None
          /\ child.state = "serving"
          /\ child.b # cur
          /\ child' = [b |-> child.b, state |-> "exited", code |-> ExitDrain]
          /\ pc' = [pc EXCEPT ![s] = "reap"]
    /\ UNCHANGED <<SupLocals, supLock, hlock, hTarget, explicitPick, cur, prev, disk, stopped, halted, rbHistory, Counts>>

\* Reap exited child and decide: stop (0), drain-restart (75), rollback, backoff-restart, or halt.
SupReap(s) ==
    /\ pc[s] = "reap"
    /\ child.state = "exited"
    /\ LET code == child.code IN
       /\ child' = DownChild
       /\ CASE code = ExitStop ->
                IF Bug = "stop_restarts"
                THEN /\ pc' = [pc EXCEPT ![s] = "check"]
                     /\ PersistDisk(sbuild[s], 0, sshunned[s], FALSE, FALSE)
                     /\ UNCHANGED <<SupLocals, stopped, halted>>
                ELSE /\ scrashes' = [scrashes EXCEPT ![s] = 0]
                     /\ PersistDisk(sbuild[s], 0, sshunned[s], FALSE, TRUE)
                     /\ stopped' = TRUE
                     /\ halted' = FALSE
                     /\ pc' = [pc EXCEPT ![s] = "unlock"]
                     /\ UNCHANGED <<sbuild, sshunned>>
            [] code = ExitDrain ->
                IF Bug = "drain_stops"
                THEN /\ stopped' = TRUE
                     /\ PersistDisk(sbuild[s], 0, sshunned[s], FALSE, TRUE)
                     /\ pc' = [pc EXCEPT ![s] = "unlock"]
                     /\ UNCHANGED <<SupLocals, halted>>
                ELSE /\ scrashes' = [scrashes EXCEPT ![s] = 0]
                     /\ PersistDisk(sbuild[s], 0, sshunned[s], FALSE, FALSE)
                     /\ pc' = [pc EXCEPT ![s] = "check"]
                     /\ UNCHANGED <<sbuild, sshunned, stopped, halted>>
            [] OTHER ->
                LET rawC == scrashes[s] + 1
                    nextC == IF Bug = "no_halt_bound" /\ rawC > MaxRestarts THEN MaxRestarts + 1 ELSE rawC
                    eligRb == (Bug # "no_post_start_rollback")
                              /\ nextC >= MaxCrashesBeforeRollback
                              /\ prev # 0
                              /\ (Bug = "ping_pong_rollback" \/ prev \notin sshunned[s])
                    mustHalt == (Bug # "no_halt_bound") /\ nextC >= MaxRestarts
                IN /\ scrashes' = [scrashes EXCEPT ![s] = nextC]
                   /\ IF eligRb
                      THEN /\ PersistDisk(sbuild[s], nextC, sshunned[s], FALSE, FALSE)
                           /\ pc' = [pc EXCEPT ![s] = "rb_lock"]
                           /\ UNCHANGED <<sbuild, sshunned, stopped, halted>>
                      ELSE IF mustHalt
                      THEN /\ PersistDisk(sbuild[s], nextC, sshunned[s], TRUE, FALSE)
                           /\ halted' = TRUE
                           /\ pc' = [pc EXCEPT ![s] = "unlock"]
                           /\ UNCHANGED <<sbuild, sshunned, stopped>>
                      ELSE /\ PersistDisk(sbuild[s], nextC, sshunned[s], FALSE, FALSE)
                           /\ pc' = [pc EXCEPT ![s] = "backoff"]
                           /\ UNCHANGED <<sbuild, sshunned, stopped, halted>>
    /\ UNCHANGED <<supLock, hlock, hTarget, explicitPick, cur, prev, rbHistory, Counts>>

SupBackoff(s) ==
    /\ pc[s] = "backoff"
    /\ pc' = [pc EXCEPT ![s] = "check"]
    /\ UNCHANGED <<SupLocals, supLock, hlock, hTarget, explicitPick, cur, prev, disk, child, stopped, halted, rbHistory, Counts>>

\* Acquire `handover/`'s exclusive lock before rolling `builds/active` back to `previous`.
SupRbLock(s) ==
    /\ pc[s] = "rb_lock"
    /\ IF Bug = "no_handover_lock"
       THEN /\ pc' = [pc EXCEPT ![s] = "rollback"]
            /\ UNCHANGED hlock
       ELSE IF hlock = None
       THEN /\ hlock' = s
            /\ pc' = [pc EXCEPT ![s] = "rollback"]
       ELSE UNCHANGED <<pc, hlock>>
    /\ UNCHANGED <<SupLocals, supLock, hTarget, explicitPick, cur, prev, disk, child, stopped, halted, rbHistory, Counts>>

\* Switch `builds/active` back to `prev` and record the failed build in `shunned` (and `rbHistory`).
SupRollback(s) ==
    /\ pc[s] = "rollback"
    /\ LET target == prev
           failed == cur
           canSwitch == target # 0 /\ target \in Tested /\ (Bug = "ping_pong_rollback" \/ target \notin sshunned[s])
           newShun == IF Bug = "ping_pong_rollback" THEN sshunned[s] ELSE sshunned[s] \cup {failed}
       IN IF canSwitch
          THEN /\ cur' = target
               /\ prev' = failed
               /\ rbHistory' = rbHistory \cup {failed}
               /\ sbuild' = [sbuild EXCEPT ![s] = target]
               /\ scrashes' = [scrashes EXCEPT ![s] = 0]
               /\ sshunned' = [sshunned EXCEPT ![s] = newShun]
               /\ PersistDisk(target, 0, newShun, FALSE, FALSE)
               /\ pc' = [pc EXCEPT ![s] = "rb_unlock"]
               /\ UNCHANGED halted
          ELSE IF Bug # "no_halt_bound" /\ scrashes[s] >= MaxRestarts
               THEN /\ PersistDisk(sbuild[s], scrashes[s], sshunned[s], TRUE, FALSE)
                    /\ halted' = TRUE
                    /\ pc' = [pc EXCEPT ![s] = "rb_unlock"]
                    /\ UNCHANGED <<cur, prev, rbHistory, SupLocals>>
               ELSE /\ pc' = [pc EXCEPT ![s] = "rb_unlock"]
                    /\ UNCHANGED <<cur, prev, rbHistory, SupLocals, disk, halted>>
    /\ UNCHANGED <<supLock, hlock, hTarget, explicitPick, child, stopped, Counts>>

SupRbUnlock(s) ==
    /\ pc[s] = "rb_unlock"
    /\ hlock' = IF hlock = s THEN None ELSE hlock
    /\ pc' = [pc EXCEPT ![s] = IF halted THEN "unlock" ELSE "check"]
    /\ UNCHANGED <<SupLocals, supLock, hTarget, explicitPick, cur, prev, disk, child, stopped, halted, rbHistory, Counts>>

SupUnlock(s) ==
    /\ pc[s] = "unlock"
    /\ supLock' = IF supLock = s THEN None ELSE supLock
    /\ pc' = [pc EXCEPT ![s] = "idle"]
    /\ ResetSup(s)
    /\ UNCHANGED <<hlock, hTarget, explicitPick, cur, prev, disk, child, stopped, halted, rbHistory, Counts>>

\* Supervisor process itself dies (e.g. SIGKILL); OS releases locks and its child ends with it; service manager
\* restarts a fresh supervisor reading `supervise/state` from disk.
SupDie(s) ==
    /\ pc[s] # "idle"
    /\ supDeaths < MaxSupDeaths
    /\ pc' = [pc EXCEPT ![s] = "idle"]
    /\ supLock' = IF supLock = s THEN None ELSE supLock
    /\ hlock' = IF hlock = s THEN None ELSE hlock
    /\ child' = IF supLock = s THEN DownChild ELSE child
    /\ supDeaths' = supDeaths + 1
    /\ ResetSup(s)
    /\ UNCHANGED <<hTarget, explicitPick, cur, prev, disk, stopped, halted, rbHistory, races, handovers, stops>>

\* Healthy serving build passes stability window: resets `crashes` to 0 while keeping `shunned`.
ChildStable(s) ==
    /\ pc[s] = "watch"
    /\ child.state = "serving"
    /\ child.b \notin (StartupBroken \cup LateBroken)
    /\ scrashes[s] > 0
    /\ scrashes' = [scrashes EXCEPT ![s] = 0]
    /\ PersistDisk(sbuild[s], 0, sshunned[s], FALSE, FALSE)
    /\ UNCHANGED <<pc, sbuild, sshunned, supLock, hlock, hTarget, explicitPick, cur, prev, child, stopped, halted, rbHistory, Counts>>

\* A serving `LateBroken` build crashes after starting.
ChildLateCrash ==
    /\ child.state = "serving"
    /\ child.b \in LateBroken
    /\ child' = [b |-> child.b, state |-> "exited", code |-> ExitCrash]
    /\ UNCHANGED <<pc, SupLocals, supLock, hlock, hTarget, explicitPick, cur, prev, disk, stopped, halted, rbHistory, Counts>>

\* Intentional `nexus-server stop` on `server.sock`: records `stopped` on disk before the server exits 0 so even if the
\* supervisor dies before reaping the child, a restarted supervisor honors the stop!
UserStop ==
    /\ stops < MaxStops
    /\ child.state = "serving"
    /\ hlock = None
    /\ child' = [b |-> child.b, state |-> "exited", code |-> ExitStop]
    /\ disk' = [disk EXCEPT !.stopped = TRUE]
    /\ stops' = stops + 1
    /\ UNCHANGED <<pc, SupLocals, supLock, hlock, hTarget, explicitPick, cur, prev, stopped, halted, rbHistory, races, supDeaths, handovers>>

\* External `nexus-server handover <t>`: acquires `hlock`, drains running child (ExitDrain = 75), switches `(cur, prev)`
\* (or leaves `cur` unchanged if `t \in StartupBroken`, matching Layer 3 Handover), and releases `hlock`.
HandoverBegin(t) ==
    /\ handovers < MaxHandovers
    /\ hlock = None
    /\ t \in Builds
    /\ (t \in Tested \/ Bug = "serve_untested")
    /\ hlock' = HandoverHolder
    /\ hTarget' = t
    /\ stopped' = FALSE
    /\ halted' = FALSE
    /\ disk' = [disk EXCEPT !.stopped = FALSE, !.halted = FALSE]
    /\ handovers' = handovers + 1
    /\ IF child.state = "serving" /\ child.b # t
       THEN child' = [b |-> child.b, state |-> "exited", code |-> ExitDrain]
       ELSE UNCHANGED child
    /\ UNCHANGED <<pc, SupLocals, supLock, explicitPick, cur, prev, rbHistory, races, supDeaths, stops>>

HandoverFinish ==
    /\ hlock = HandoverHolder
    /\ LET t == hTarget IN
       /\ IF t \in StartupBroken
          THEN /\ UNCHANGED <<cur, prev, explicitPick, rbHistory, disk>>
          ELSE /\ IF cur # t
                  THEN /\ prev' = cur
                       /\ cur' = t
                  ELSE UNCHANGED <<cur, prev>>
               /\ explicitPick' = t
               /\ rbHistory' = rbHistory \ {t}
               /\ IF Bug = "in_memory_sup_state"
                  THEN UNCHANGED disk
                  ELSE disk' = [build |-> t, crashes |-> 0, shunned |-> disk.shunned \ {t},
                                halted |-> FALSE, stopped |-> FALSE]
       /\ hlock' = None
       /\ hTarget' = 0
    /\ UNCHANGED <<pc, SupLocals, supLock, child, stopped, halted, Counts>>

SupStep(s) ==
    \/ SupLockTry(s) \/ SupLockGiveUp(s) \/ SupLoad(s) \/ SupCheck(s)
    \/ SupSpawn(s) \/ SupWatch(s) \/ SupReap(s) \/ SupBackoff(s)
    \/ SupRbLock(s) \/ SupRollback(s) \/ SupRbUnlock(s) \/ SupUnlock(s)
    \/ ChildStable(s)

AnyEnsureSup == \E s \in SupActors : EnsureSup(s)

Done ==
    /\ \A s \in SupActors : pc[s] \in {"idle", "watch"}
    /\ hlock = None
    /\ (stopped \/ halted \/ (child.state = "serving" /\ child.b = cur /\ child.b \notin (StartupBroken \cup LateBroken)))
    /\ UNCHANGED vars

Next ==
    \/ AnyEnsureSup
    \/ \E s \in SupActors : RaceSupStart(s) \/ SupStep(s) \/ SupDie(s)
    \/ ChildLateCrash
    \/ UserStop
    \/ \E t \in Builds : HandoverBegin(t)
    \/ HandoverFinish
    \/ Done

Spec == Init /\ [][Next]_vars

Live ==
    /\ Spec
    /\ WF_vars(AnyEnsureSup)
    /\ (\A s \in SupActors : WF_vars(SupStep(s)))
    /\ WF_vars(HandoverFinish)
    /\ WF_vars(ChildLateCrash)

\* Spec variant where at least one handover occurs, to prove post-start failure rollback end-to-end.
AnyHandover == \E t \in (Builds \ {Initial}) : HandoverBegin(t)
RollbackLive ==
    /\ Live
    /\ WF_vars(AnyHandover)

-----------------------------------------------------------------------------
\* Properties

AllPcs == {"idle", "lock", "load", "check", "spawn", "watch", "reap", "backoff",
           "rb_lock", "rollback", "rb_unlock", "unlock"}

TypeOK ==
    /\ pc \in [SupActors -> AllPcs]
    /\ supLock \in SupActors \cup {None}
    /\ hlock \in SupActors \cup {HandoverHolder, None}
    /\ cur \in Builds
    /\ prev \in Builds \cup {0}
    /\ child.b \in Builds \cup {0}
    /\ child.state \in {"down", "serving", "exited"}
    /\ stopped \in BOOLEAN
    /\ halted \in BOOLEAN

\* At most one supervisor runs past `"lock"`, and that actor holds `supLock`.
OneSupervisor ==
    /\ Cardinality({s \in SupActors : pc[s] \notin {"idle", "lock"}}) <= 1
    /\ \A s \in SupActors : pc[s] \notin {"idle", "lock"} => supLock = s

\* Supervisor rollback and external handover never race on `builds/active` (`hlock` is held by every rollback).
HandoverSerialized ==
    \A s \in SupActors : pc[s] \in {"rollback", "rb_unlock"} => hlock = s

\* `current` is always a tested build.
CurrentTested == cur \in Tested

\* Consecutive crashes without halting never exceed `MaxRestarts`.
RestartsBounded ==
    /\ \A s \in SupActors : scrashes[s] <= MaxRestarts
    /\ disk.crashes <= MaxRestarts

\* Supervision never rolls `current` back to a build that already failed and was rolled back from.
NeverRollsBackToShunned ==
    [][\A s \in SupActors : (pc[s] = "rollback" /\ cur' # cur) => cur' \notin rbHistory]_vars

\* Reaping an intentional `stop` (0) marks the service `stopped`, and while `disk.stopped` holds no child spawns (even
\* across supervisor restarts!).
StopStaysStopped ==
    [][
        /\ ((child.state = "exited" /\ child.code = ExitStop /\ child'.state = "down" /\ supDeaths' = supDeaths) => stopped')
        /\ ((disk.stopped /\ disk'.stopped /\ child.state = "down") => child'.state = "down")
    ]_vars

\* Reaping a `drain` exit (75) never marks the service `stopped`.
DrainRestartsCurrent ==
    [][(child.state = "exited" /\ child.code = ExitDrain /\ child'.state = "down" /\ supDeaths' = supDeaths) => ~stopped']_vars

\* Supervision always settles without spinning: serving a healthy tested build, intentionally stopped, or halted.
SupervisorSettles ==
    <>[](
        /\ hlock = None
        /\ \/ (child.state = "serving" /\ child.b = cur /\ cur \in (Tested \ (StartupBroken \cup LateBroken)))
           \/ (stopped /\ child.state = "down" /\ \A s \in SupActors : pc[s] = "idle")
           \/ (halted /\ child.state = "down" /\ \A s \in SupActors : pc[s] = "idle")
    )

\* Post-start failure rollback: when `Initial` is healthy and no `UserStop` occurs, handover to a `LateBroken` build
\* starts, crashes after serving, rolls `cur` back to `Initial`, and settles serving `Initial`!
SettledHealthy ==
    <>([] (child.state = "serving" /\ child.b = cur /\ cur \in (Tested \ (StartupBroken \cup LateBroken)) /\ ~stopped /\ ~halted))

=============================================================================
