------------------------------ MODULE Takeover ------------------------------
(***************************************************************************)
(* End-to-end takeover composition (docs/roadmap.sexp, `takeover-binding`, *)
(* layer 6 of 6): binds all five layers operating together on one server   *)
(* home across upgrades, drains, post-start crashes, format bumps,         *)
(* rollbacks, process cuts, and old-layout migration:                      *)
(*                                                                         *)
(*   Layer 1 (StoreLock):  exclusive store lock held while serving or      *)
(*                         draining; shared lock for disk-format checks.   *)
(*   Layer 2 (Placement):  immutable builds placed and verified by hash;   *)
(*                         (current, previous) switched in one rename.     *)
(*   Layer 3 (Handover):   exclusive flock on handover/, durable stage     *)
(*                         records (handover/state), open-turn drain, and  *)
(*                         cut-anywhere resume.                            *)
(*   Layer 4 (Supervision):exclusive flock on supervise/, durable crash    *)
(*                         counts & shunned set (supervise/state),         *)
(*                         seamless drain (75), honored stop (0), and      *)
(*                         post-start rollback / bounded halt (78).        *)
(*   Layer 5 (Migration):  one-time ~/.nexus -> ~/.nexus/server move       *)
(*                         refusing live old servers, and disk-format      *)
(*                         reader checks (Builds::rollbackTo, Store)       *)
(*                         across rollback and multi-hop downgrades.       *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    AllowedBuilds,            \* subset of {1, 2, 3}: 1=(3/3), 2=(5/3 compat), 3=(5/5 breaking)
    InitPlaced,               \* subset of AllowedBuilds initially placed
    InitTested,               \* subset of InitPlaced initially tested
    InitCur,                  \* 0 or an element of InitTested
    InitOldLayout,            \* "none", "cold" (~/.nexus files), or "live" (answers server.sock)
    StartupBroken,            \* subset of AllowedBuilds that fail before reaching SERVING
    LateBroken,               \* subset of AllowedBuilds that reach SERVING, then crash later
    Format5Writers,           \* subset of AllowedBuilds that write format-5 records once serving
    MaxCrashesBeforeRollback, \* crashes on `cur` before supervisor rolls back to `prev`
    MaxRestarts,              \* crashes without an eligible rollback before supervisor halts (78)
    MaxHandovers,             \* bound on explicit `handover <target>` commands
    MaxCuts,                  \* bound on mid-handover process kills
    MaxSupDeaths,             \* bound on supervisor process kills
    MaxTurns,                 \* bound on turns / store writes
    MaxStops,                 \* bound on intentional `nexus-server stop` requests
    Bug                       \* "none", or one reversed witness break

ASSUME AllowedBuilds \subseteq {1, 2, 3}
ASSUME InitPlaced \subseteq AllowedBuilds /\ InitTested \subseteq InitPlaced
ASSUME InitCur = 0 \/ InitCur \in InitTested
ASSUME InitOldLayout \in {"none", "cold", "live"}
ASSUME StartupBroken \subseteq AllowedBuilds /\ LateBroken \subseteq AllowedBuilds
ASSUME Format5Writers \subseteq AllowedBuilds
ASSUME MaxCrashesBeforeRollback >= 1 /\ MaxRestarts >= MaxCrashesBeforeRollback

\* Format table of the three builds:
\*   Build 1: writes 3, reader 3 (base)
\*   Build 2: writes 5, reader 3 (appended fields & unknown change kinds; rollback-compatible with 1)
\*   Build 3: writes 5, reader 5 (raises reader floor to 5 once it writes to the store)
Writes(b) == IF b = 1 THEN 3 ELSE 5
Reader(b) == IF b = 3 THEN 5 ELSE 3

ExitStop == 0
ExitCrash == 1
ExitDrain == 75

NoHState == [stage |-> "none", target |-> 0, previous |-> 0]
DownSrv == [state |-> "down", b |-> 0, turn |-> FALSE, code |-> 0]

VARIABLES
    oldLayout,      \* "none", "cold", or "live"
    migrated,       \* BOOLEAN: old-layout migration completed (or started "none")
    placed,         \* subset of AllowedBuilds in <dir>/builds
    tested,         \* subset of placed with passing test evidence
    cur,            \* 0 or active current build
    prev,           \* 0 or active previous build
    hlock,          \* "none", "ho" (handover CLI), or "sup" (supervisor rollback)
    hstate,         \* durable <dir>/handover/state: [stage, target, previous]
    supLock,        \* BOOLEAN: supervisor holds <dir>/supervise flock
    supPc,          \* "idle", "check", "watch", "reap", "rollback"
    supDisk,        \* durable <dir>/supervise/: [build, crashes, shunned, halted, stopped]
    explicitPick,   \* 0 or build recorded by explicit handover (`supervise/picked`)
    srv,            \* [state \in {"down","serving","draining","exited"}, b, turn, code]
    maxReader,      \* 0, 3, or 5: highest reader byte in <dir>/store on disk
    sessions,       \* Nat: durable entries/sessions in <dir>/store
    wroteBreaking,  \* BOOLEAN ghost: a breaking build wrote reader = 5 to <dir>/store
    handovers, cuts, supDeaths, turns, stops

vars == <<oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
          supLock, supPc, supDisk, explicitPick, srv, maxReader, sessions,
          wroteBreaking, handovers, cuts, supDeaths, turns, stops>>

Counts == <<handovers, cuts, supDeaths, turns, stops>>

Init ==
    /\ oldLayout = InitOldLayout
    /\ migrated = (InitOldLayout = "none")
    /\ placed = InitPlaced
    /\ tested = InitTested
    /\ cur = InitCur
    /\ prev = 0
    /\ hlock = "none"
    /\ hstate = NoHState
    /\ supLock = FALSE
    /\ supPc = "idle"
    /\ supDisk = [build |-> 0, crashes |-> 0, shunned |-> {}, halted |-> FALSE, stopped |-> FALSE]
    /\ explicitPick = 0
    /\ srv = DownSrv
    /\ maxReader = 0
    /\ sessions = 0
    /\ wroteBreaking = FALSE
    /\ handovers = 0
    /\ cuts = 0
    /\ supDeaths = 0
    /\ turns = 0
    /\ stops = 0

\* Whether build `b` can open `<dir>/store` and reach SERVING.
CanServe(b) ==
    /\ b # 0
    /\ (oldLayout = "none" \/ Bug = "skip_migration")
    /\ b \notin StartupBroken
    /\ maxReader <= Writes(b)

\* Layer 5 rollback format check (`Builds::rollbackTo`):
\*   - Normal (`Bug = "none"`): inspects disk `maxReader <= Writes(target)`.
\*   - `Bug = "blind_rollback"`: skips disk check (allows switching to unreadable build!).
\*   - `Bug = "version_only_rollback"`: uses `formatsRefusal` (`Writes(target) >= Reader(from)`) instead of disk check,
\*     falsely refusing rollback from a `5/5` build that crashed before writing format 5!
RollbackFormatOk(target, from) ==
    IF Bug = "blind_rollback" THEN TRUE
    ELSE IF Bug = "version_only_rollback" THEN Writes(target) >= Reader(from)
    ELSE maxReader <= Writes(target)

-----------------------------------------------------------------------------
(* Layer 5: Old-layout stop & migration                                    *)
-----------------------------------------------------------------------------

OldServerStop ==
    /\ oldLayout = "live"
    /\ oldLayout' = "cold"
    /\ UNCHANGED <<migrated, placed, tested, cur, prev, hlock, hstate,
                   supLock, supPc, supDisk, explicitPick, srv, maxReader, sessions,
                   wroteBreaking, Counts>>

Migrate ==
    /\ oldLayout # "none"
    /\ srv.state = "down"
    /\ IF oldLayout = "live" /\ Bug # "ignore_live_old"
       THEN UNCHANGED <<oldLayout, migrated, maxReader, sessions>>
       ELSE /\ oldLayout' = "none"
            /\ migrated' = TRUE
            /\ maxReader' = IF maxReader < 3 THEN 3 ELSE maxReader
            /\ sessions' = IF sessions < 1 THEN 1 ELSE sessions
    /\ UNCHANGED <<placed, tested, cur, prev, hlock, hstate,
                   supLock, supPc, supDisk, explicitPick, srv, wroteBreaking, Counts>>

-----------------------------------------------------------------------------
(* Layer 2: Placing builds and choosing the first current build            *)
-----------------------------------------------------------------------------

PlaceBuild(b, isTested) ==
    /\ b \in AllowedBuilds \ placed
    /\ placed' = placed \cup {b}
    /\ tested' = IF isTested THEN tested \cup {b} ELSE tested
    /\ UNCHANGED <<oldLayout, migrated, cur, prev, hlock, hstate,
                   supLock, supPc, supDisk, explicitPick, srv, maxReader, sessions,
                   wroteBreaking, Counts>>

SetInitialCurrent(b) ==
    /\ cur = 0
    /\ hlock = "none"
    /\ hstate.stage = "none"
    /\ b \in placed
    /\ (b \in tested \/ Bug = "serve_untested")
    /\ cur' = b
    /\ prev' = 0
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, hlock, hstate,
                   supLock, supPc, supDisk, explicitPick, srv, maxReader, sessions,
                   wroteBreaking, Counts>>

-----------------------------------------------------------------------------
(* Layer 3: Handover (`nexus-server handover <target>` & `resume`)         *)
-----------------------------------------------------------------------------

HoBegin(t) ==
    /\ handovers < MaxHandovers
    /\ hlock = "none"
    /\ hstate.stage = "none"
    /\ cur # 0
    /\ t \in placed \ {cur}
    /\ (t \in tested \/ Bug = "serve_untested")
    /\ oldLayout = "none"
    \* Forward version rule + Layer 5 multi-hop disk check when downgrading writes:
    /\ Writes(t) >= Reader(cur)
    /\ IF Writes(t) < Writes(cur)
       THEN srv.state \notin {"serving", "draining"} /\ maxReader <= Writes(t)
       ELSE TRUE
    /\ hlock' = IF Bug = "no_handover_lock" THEN "none" ELSE "ho"
    /\ hstate' = [stage |-> "draining", target |-> t, previous |-> cur]
    /\ prev' = cur
    /\ cur' = t
    /\ supDisk' = [supDisk EXCEPT !.stopped = FALSE, !.halted = FALSE]
    /\ handovers' = handovers + 1
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, supLock, supPc,
                   explicitPick, srv, maxReader, sessions, wroteBreaking,
                   cuts, supDeaths, turns, stops>>

HoResumeAcquire ==
    /\ hlock = "none"
    /\ hstate.stage # "none"
    /\ hlock' = IF Bug = "no_handover_lock" THEN "none" ELSE "ho"
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hstate,
                   supLock, supPc, supDisk, explicitPick, srv, maxReader, sessions,
                   wroteBreaking, Counts>>

HoDrain ==
    /\ (hlock = "ho" \/ (Bug = "no_handover_lock" /\ hstate.stage = "draining"))
    /\ hstate.stage = "draining"
    /\ IF srv.state = "serving" /\ srv.b # hstate.target
       THEN IF Bug = "drain_cuts_turn"
            THEN \* Faulty: cuts an open turn immediately via stop/kill instead of draining!
                 /\ srv' = IF supLock /\ supPc = "watch"
                           THEN [state |-> "exited", b |-> srv.b, turn |-> FALSE, code |-> ExitDrain]
                           ELSE DownSrv
                 /\ hstate' = [hstate EXCEPT !.stage = "starting"]
            ELSE IF ~srv.turn
                 THEN /\ srv' = IF supLock /\ supPc = "watch"
                                THEN [state |-> "exited", b |-> srv.b, turn |-> FALSE, code |-> ExitDrain]
                                ELSE DownSrv
                      /\ hstate' = [hstate EXCEPT !.stage = "starting"]
                 ELSE \* Open turn: server enters draining and finishes turn before exiting 75!
                      /\ srv' = [srv EXCEPT !.state = "draining"]
                      /\ UNCHANGED hstate
       ELSE IF srv.state = "draining"
            THEN UNCHANGED <<srv, hstate>>
            ELSE /\ UNCHANGED srv
                 /\ hstate' = [hstate EXCEPT !.stage = "starting"]
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hlock,
                   supLock, supPc, supDisk, explicitPick, maxReader, sessions,
                   wroteBreaking, Counts>>

HoStart ==
    /\ (hlock = "ho" \/ (Bug = "no_handover_lock" /\ hstate.stage = "starting"))
    /\ hstate.stage = "starting"
    /\ LET rtgt == hstate.target
           rprev == hstate.previous
       IN \/ /\ srv.state = "serving"
             /\ srv.b = rtgt
             /\ UNCHANGED <<srv, maxReader>>
             /\ explicitPick' = rtgt
             /\ supDisk' = [supDisk EXCEPT !.build = rtgt, !.crashes = 0, !.shunned = @ \ {rtgt},
                                           !.halted = FALSE, !.stopped = FALSE]
             /\ hstate' = NoHState
             /\ hlock' = "none"
          \/ /\ srv.state = "down"
             /\ IF CanServe(rtgt)
                THEN /\ srv' = [state |-> "serving", b |-> rtgt, turn |-> FALSE, code |-> 0]
                     /\ maxReader' = IF maxReader = 0 THEN 3 ELSE maxReader
                     /\ explicitPick' = rtgt
                     /\ supDisk' = [supDisk EXCEPT !.build = rtgt, !.crashes = 0, !.shunned = @ \ {rtgt},
                                                   !.halted = FALSE, !.stopped = FALSE]
                     /\ hstate' = NoHState
                     /\ hlock' = "none"
                ELSE \* Target failed before reaching SERVING: roll back to `rprev` if present!
                     /\ UNCHANGED <<srv, maxReader, explicitPick, supDisk>>
                     /\ IF rprev = 0
                        THEN /\ hstate' = NoHState
                             /\ hlock' = "none"
                        ELSE /\ hstate' = [hstate EXCEPT !.stage = "rolling-back"]
                             /\ UNCHANGED hlock
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev,
                   supLock, supPc, sessions, wroteBreaking, Counts>>

HoRollback ==
    /\ (hlock = "ho" \/ (Bug = "no_handover_lock" /\ hstate.stage = "rolling-back"))
    /\ hstate.stage = "rolling-back"
    /\ srv.state = "down"
    /\ LET rtgt == hstate.target
           rprev == hstate.previous
       IN /\ IF rprev # 0 /\ rprev \in tested /\ RollbackFormatOk(rprev, cur)
             THEN /\ cur' = rprev
                  /\ prev' = cur
                  /\ IF CanServe(rprev)
                     THEN /\ srv' = [state |-> "serving", b |-> rprev, turn |-> FALSE, code |-> 0]
                          /\ maxReader' = IF maxReader = 0 THEN 3 ELSE maxReader
                     ELSE UNCHANGED <<srv, maxReader>>
             ELSE UNCHANGED <<cur, prev, srv, maxReader>>
          /\ hstate' = NoHState
          /\ hlock' = "none"
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, supLock, supPc, supDisk,
                   explicitPick, sessions, wroteBreaking, Counts>>

HoCut ==
    /\ hlock = "ho"
    /\ hstate.stage # "none"
    /\ cuts < MaxCuts
    /\ hlock' = "none"
    /\ cuts' = cuts + 1
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hstate,
                   supLock, supPc, supDisk, explicitPick, srv, maxReader, sessions,
                   wroteBreaking, handovers, supDeaths, turns, stops>>

-----------------------------------------------------------------------------
(* Layer 1 & Server runtime: turns, format-5 writes, crashes, stops        *)
-----------------------------------------------------------------------------

TurnOpen ==
    /\ srv.state = "serving"
    /\ ~srv.turn
    /\ turns < MaxTurns
    /\ srv' = [srv EXCEPT !.turn = TRUE]
    /\ turns' = turns + 1
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
                   supLock, supPc, supDisk, explicitPick, maxReader, sessions,
                   wroteBreaking, handovers, cuts, supDeaths, stops>>

TurnComplete ==
    /\ srv.state \in {"serving", "draining"}
    /\ srv.turn
    /\ LET wrote5 == srv.b \in Format5Writers /\ Reader(srv.b) = 5
           nextR == IF srv.b \in Format5Writers /\ Reader(srv.b) > maxReader
                    THEN Reader(srv.b)
                    ELSE IF maxReader = 0 THEN 3 ELSE maxReader
       IN /\ sessions' = sessions + 1
          /\ maxReader' = nextR
          /\ wroteBreaking' = (wroteBreaking \/ wrote5)
          /\ IF srv.state = "draining"
             THEN srv' = IF supLock /\ supPc = "watch"
                         THEN [state |-> "exited", b |-> srv.b, turn |-> FALSE, code |-> ExitDrain]
                         ELSE DownSrv
             ELSE srv' = [srv EXCEPT !.turn = FALSE]
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
                   supLock, supPc, supDisk, explicitPick, Counts>>

SrvWriteFormat ==
    /\ srv.state = "serving"
    /\ ~srv.turn
    /\ srv.b \in Format5Writers
    /\ turns < MaxTurns
    /\ LET wrote5 == Reader(srv.b) = 5
           nextR == IF Reader(srv.b) > maxReader THEN Reader(srv.b)
                    ELSE IF maxReader = 0 THEN 3 ELSE maxReader
       IN /\ sessions' = sessions + 1
          /\ maxReader' = nextR
          /\ wroteBreaking' = (wroteBreaking \/ wrote5)
          /\ turns' = turns + 1
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
                   supLock, supPc, supDisk, explicitPick, srv,
                   handovers, cuts, supDeaths, stops>>

SrvLateCrash ==
    /\ srv.state = "serving"
    /\ ~srv.turn
    /\ srv.b \in LateBroken
    /\ srv' = IF supLock /\ supPc = "watch"
              THEN [state |-> "exited", b |-> srv.b, turn |-> FALSE, code |-> ExitCrash]
              ELSE DownSrv
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
                   supLock, supPc, supDisk, explicitPick, maxReader, sessions,
                   wroteBreaking, Counts>>

UserStop ==
    /\ stops < MaxStops
    /\ srv.state = "serving"
    /\ ~srv.turn
    /\ (~supLock \/ supPc = "watch")
    /\ hlock = "none"
    /\ hstate.stage = "none"
    /\ supDisk' = [supDisk EXCEPT !.stopped = TRUE]
    /\ srv' = IF supLock /\ supPc = "watch"
              THEN [state |-> "exited", b |-> srv.b, turn |-> FALSE, code |-> ExitStop]
              ELSE DownSrv
    /\ stops' = stops + 1
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
                   supLock, supPc, explicitPick, maxReader, sessions, wroteBreaking,
                   handovers, cuts, supDeaths, turns>>

-----------------------------------------------------------------------------
(* Layer 4: Supervision (`nexus-server supervise`)                         *)
-----------------------------------------------------------------------------

SupStart ==
    /\ ~supLock
    /\ cur # 0
    /\ (~supDisk.stopped \/ Bug = "stop_restarts")
    /\ (~supDisk.halted \/ supDisk.build # cur \/ explicitPick = cur)
    /\ IF oldLayout = "live"
       THEN IF Bug = "blame_build_for_live_old"
            THEN \* Faulty: spawns `serve` repeatedly while live old server holds ~/.nexus/server.sock and halts!
                 /\ supDisk' = [supDisk EXCEPT !.build = cur, !.crashes = MaxRestarts, !.halted = TRUE]
                 /\ UNCHANGED <<oldLayout, migrated, supLock, supPc, maxReader, sessions>>
            ELSE IF Bug = "ignore_live_old"
            THEN /\ oldLayout' = "none"
                 /\ migrated' = TRUE
                 /\ maxReader' = IF maxReader < 3 THEN 3 ELSE maxReader
                 /\ sessions' = IF sessions < 1 THEN 1 ELSE sessions
                 /\ supLock' = TRUE
                 /\ supPc' = "check"
                 /\ UNCHANGED supDisk
            ELSE \* Clean refusal (exit 1) before touching supervise/state; systemd retries once old server stops.
                 UNCHANGED <<oldLayout, migrated, supLock, supPc, supDisk, maxReader, sessions>>
       ELSE /\ IF oldLayout = "cold" /\ Bug # "skip_migration"
               THEN /\ oldLayout' = "none"
                    /\ migrated' = TRUE
                    /\ maxReader' = IF maxReader < 3 THEN 3 ELSE maxReader
                    /\ sessions' = IF sessions < 1 THEN 1 ELSE sessions
               ELSE UNCHANGED <<oldLayout, migrated, maxReader, sessions>>
            /\ supLock' = TRUE
            /\ supPc' = "check"
            /\ UNCHANGED supDisk
    /\ UNCHANGED <<placed, tested, cur, prev, hlock, hstate, explicitPick, srv, wroteBreaking, Counts>>

SupCheck ==
    /\ supLock
    /\ supPc = "check"
    /\ IF hlock = "ho" \/ hstate.stage # "none"
       THEN UNCHANGED <<hlock, supLock, supPc, supDisk, explicitPick, srv, maxReader>>
       ELSE IF supDisk.stopped /\ Bug # "stop_restarts"
       THEN /\ supLock' = FALSE
            /\ supPc' = "idle"
            /\ UNCHANGED <<hlock, supDisk, explicitPick, srv, maxReader>>
       ELSE IF supDisk.halted /\ supDisk.build = cur /\ explicitPick # cur
       THEN /\ supLock' = FALSE
            /\ supPc' = "idle"
            /\ UNCHANGED <<hlock, supDisk, explicitPick, srv, maxReader>>
       ELSE IF cur \notin tested /\ Bug # "serve_untested"
       THEN /\ supDisk' = [supDisk EXCEPT !.build = cur, !.halted = TRUE]
            /\ explicitPick' = IF explicitPick = cur THEN 0 ELSE explicitPick
            /\ supLock' = FALSE
            /\ supPc' = "idle"
            /\ UNCHANGED <<hlock, srv, maxReader>>
       ELSE IF srv.state = "exited"
       THEN /\ supPc' = "reap"
            /\ UNCHANGED <<hlock, supLock, supDisk, explicitPick, srv, maxReader>>
       ELSE LET picked == (explicitPick = cur)
                newShun == IF picked THEN supDisk.shunned \ {cur} ELSE supDisk.shunned
                newCrashes == IF supDisk.build = cur /\ ~picked THEN supDisk.crashes ELSE 0
                eligRb == newCrashes >= MaxCrashesBeforeRollback /\ prev # 0 /\ prev \notin newShun
                mustHalt == newCrashes >= MaxRestarts
            IN /\ explicitPick' = IF picked THEN 0 ELSE explicitPick
               /\ IF srv.state = "serving" /\ srv.b = cur
                  THEN /\ supDisk' = [build |-> cur, crashes |-> newCrashes, shunned |-> newShun,
                                      halted |-> FALSE, stopped |-> FALSE]
                       /\ supPc' = "watch"
                       /\ UNCHANGED <<hlock, supLock, srv, maxReader>>
                  ELSE IF srv.state = "serving" /\ srv.b # cur
                  THEN \* Drain stale serving build before spawning `cur`
                       /\ supDisk' = [build |-> cur, crashes |-> newCrashes, shunned |-> newShun,
                                      halted |-> FALSE, stopped |-> FALSE]
                       /\ srv' = IF ~srv.turn
                                 THEN [state |-> "exited", b |-> srv.b, turn |-> FALSE, code |-> ExitDrain]
                                 ELSE [srv EXCEPT !.state = "draining"]
                       /\ supPc' = "watch"
                       /\ UNCHANGED <<hlock, supLock, maxReader>>
                  ELSE IF srv.state = "draining"
                  THEN /\ supDisk' = [build |-> cur, crashes |-> newCrashes, shunned |-> newShun,
                                      halted |-> FALSE, stopped |-> FALSE]
                       /\ supPc' = "watch"
                       /\ UNCHANGED <<hlock, supLock, srv, maxReader>>
                  ELSE IF eligRb
                  THEN /\ supDisk' = [build |-> cur, crashes |-> newCrashes, shunned |-> newShun,
                                      halted |-> FALSE, stopped |-> FALSE]
                       /\ hlock' = IF Bug = "no_handover_lock" THEN hlock ELSE "sup"
                       /\ supPc' = "rollback"
                       /\ UNCHANGED <<supLock, srv, maxReader>>
                  ELSE IF mustHalt
                  THEN /\ supDisk' = [build |-> cur, crashes |-> newCrashes, shunned |-> newShun,
                                      halted |-> TRUE, stopped |-> FALSE]
                       /\ supLock' = FALSE
                       /\ supPc' = "idle"
                       /\ UNCHANGED <<hlock, srv, maxReader>>
                  ELSE \* Spawn `cur`!
                       /\ supDisk' = [build |-> cur, crashes |-> newCrashes, shunned |-> newShun,
                                      halted |-> FALSE, stopped |-> FALSE]
                       /\ IF CanServe(cur)
                          THEN /\ srv' = [state |-> "serving", b |-> cur, turn |-> FALSE, code |-> 0]
                               /\ maxReader' = IF maxReader = 0 THEN 3 ELSE maxReader
                               /\ supPc' = "watch"
                          ELSE /\ srv' = [state |-> "exited", b |-> cur, turn |-> FALSE, code |-> ExitCrash]
                               /\ UNCHANGED maxReader
                               /\ supPc' = "reap"
                       /\ UNCHANGED <<hlock, supLock>>
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hstate, sessions, wroteBreaking, Counts>>

SupWatch ==
    /\ supLock
    /\ supPc = "watch"
    /\ \/ /\ srv.state = "exited"
          /\ supPc' = "reap"
          /\ UNCHANGED srv
       \/ /\ srv.state = "down"
          /\ supPc' = "check"
          /\ UNCHANGED srv
       \/ /\ hlock = "none"
          /\ hstate.stage = "none"
          /\ srv.state = "serving"
          /\ srv.b # cur
          /\ srv' = IF ~srv.turn
                    THEN [state |-> "exited", b |-> srv.b, turn |-> FALSE, code |-> ExitDrain]
                    ELSE [srv EXCEPT !.state = "draining"]
          /\ supPc' = IF ~srv.turn THEN "reap" ELSE "watch"
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hlock, hstate,
                   supLock, supDisk, explicitPick, maxReader, sessions, wroteBreaking, Counts>>

SupReap ==
    /\ supLock
    /\ supPc = "reap"
    /\ srv.state = "exited"
    /\ LET code == srv.code IN
       /\ srv' = DownSrv
       /\ CASE code = ExitStop ->
                IF Bug = "stop_restarts"
                THEN /\ supDisk' = [supDisk EXCEPT !.crashes = 0, !.stopped = FALSE]
                     /\ supPc' = "check"
                     /\ UNCHANGED <<hlock, supLock>>
                ELSE /\ supDisk' = [supDisk EXCEPT !.crashes = 0, !.stopped = TRUE, !.halted = FALSE]
                     /\ supLock' = FALSE
                     /\ supPc' = "idle"
                     /\ UNCHANGED hlock
            [] code = ExitDrain ->
                IF Bug = "drain_stops"
                THEN /\ supDisk' = [supDisk EXCEPT !.crashes = 0, !.stopped = TRUE]
                     /\ supLock' = FALSE
                     /\ supPc' = "idle"
                     /\ UNCHANGED hlock
                ELSE /\ supDisk' = [supDisk EXCEPT !.crashes = 0]
                     /\ supPc' = "check"
                     /\ UNCHANGED <<hlock, supLock>>
            [] OTHER ->
                LET nextC == supDisk.crashes + 1
                    eligRb == nextC >= MaxCrashesBeforeRollback /\ prev # 0 /\ prev \notin supDisk.shunned /\
                              hlock = "none" /\ hstate.stage = "none"
                    mustHalt == nextC >= MaxRestarts
                IN IF eligRb
                   THEN /\ supDisk' = [supDisk EXCEPT !.crashes = nextC]
                        /\ hlock' = IF Bug = "no_handover_lock" THEN hlock ELSE "sup"
                        /\ supPc' = "rollback"
                        /\ UNCHANGED supLock
                   ELSE IF mustHalt
                   THEN /\ supDisk' = [supDisk EXCEPT !.crashes = nextC, !.halted = TRUE]
                        /\ supLock' = FALSE
                        /\ supPc' = "idle"
                        /\ UNCHANGED hlock
                   ELSE /\ supDisk' = [supDisk EXCEPT !.crashes = nextC]
                        /\ supPc' = "check"
                        /\ UNCHANGED <<hlock, supLock>>
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hstate,
                   explicitPick, maxReader, sessions, wroteBreaking, Counts>>

SupRollback ==
    /\ supLock
    /\ supPc = "rollback"
    /\ LET target == prev
           failed == cur
           canSwitch == target # 0 /\ target \in tested /\ target \notin supDisk.shunned /\
                        RollbackFormatOk(target, failed)
       IN IF canSwitch
          THEN LET newShun == supDisk.shunned \cup {failed} IN
               /\ cur' = target
               /\ prev' = failed
               /\ supDisk' = [supDisk EXCEPT !.build = target, !.crashes = 0,
                                             !.shunned = newShun, !.halted = FALSE]
               /\ hlock' = IF hlock = "sup" THEN "none" ELSE hlock
               /\ supPc' = "check"
               /\ UNCHANGED supLock
          ELSE \* Rollback to `target` refused (e.g. disk store requires reader 5 > Writes(target) = 3, or untested):
               \* shun `target` so supervisor never spins in `check -> rollback -> check` and never spawns `target`!
               LET shunAfter == IF Bug = "spin_on_format_refusal" \/ target = 0
                                THEN supDisk.shunned
                                ELSE supDisk.shunned \cup {target}
               IN IF supDisk.crashes >= MaxRestarts
                  THEN /\ supDisk' = [supDisk EXCEPT !.shunned = shunAfter, !.halted = TRUE]
                       /\ hlock' = IF hlock = "sup" THEN "none" ELSE hlock
                       /\ supLock' = FALSE
                       /\ supPc' = "idle"
                       /\ UNCHANGED <<cur, prev>>
                  ELSE /\ supDisk' = [supDisk EXCEPT !.shunned = shunAfter]
                       /\ hlock' = IF hlock = "sup" THEN "none" ELSE hlock
                       /\ supPc' = "check"
                       /\ UNCHANGED <<cur, prev, supLock>>
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, hstate, explicitPick,
                   srv, maxReader, sessions, wroteBreaking, Counts>>

SupDie ==
    /\ supLock
    /\ supDeaths < MaxSupDeaths
    /\ supLock' = FALSE
    /\ supPc' = "idle"
    /\ hlock' = IF hlock = "sup" THEN "none" ELSE hlock
    /\ srv' = IF srv.state \in {"serving", "draining", "exited"} THEN DownSrv ELSE srv
    /\ supDeaths' = supDeaths + 1
    /\ UNCHANGED <<oldLayout, migrated, placed, tested, cur, prev, hstate,
                   supDisk, explicitPick, maxReader, sessions, wroteBreaking,
                   handovers, cuts, turns, stops>>

-----------------------------------------------------------------------------
(* End-state stuttering & Fairness                                         *)
-----------------------------------------------------------------------------

Settled ==
    /\ oldLayout = "none"
    /\ hstate.stage = "none"
    /\ hlock = "none"
    /\ \/ (cur = 0 /\ ~supLock /\ srv.state = "down")
       \/ (supDisk.stopped /\ ~supLock /\ srv.state = "down")
       \/ (supDisk.halted /\ ~supLock /\ srv.state = "down")
       \/ (srv.state = "serving" /\ ~srv.turn /\ srv.b = cur /\ srv.b \notin (StartupBroken \cup LateBroken))

Done ==
    /\ Settled
    /\ UNCHANGED vars

SupProgress == SupCheck \/ SupWatch \/ SupReap \/ SupRollback
HoProgress == HoResumeAcquire \/ HoDrain \/ HoStart \/ HoRollback

Next ==
    \/ OldServerStop
    \/ Migrate
    \/ \E b \in AllowedBuilds : PlaceBuild(b, TRUE) \/ PlaceBuild(b, FALSE) \/ SetInitialCurrent(b) \/ HoBegin(b)
    \/ HoProgress
    \/ HoCut
    \/ TurnOpen
    \/ TurnComplete
    \/ SrvWriteFormat
    \/ SrvLateCrash
    \/ UserStop
    \/ SupStart
    \/ SupProgress
    \/ SupDie
    \/ Done

Spec == Init /\ [][Next]_vars

Fairness ==
    /\ WF_vars(OldServerStop)
    /\ WF_vars(Migrate)
    /\ WF_vars(HoProgress)
    /\ WF_vars(TurnComplete)
    /\ WF_vars(SrvLateCrash)
    /\ WF_vars(SupStart)
    /\ WF_vars(SupProgress)

Live == Spec /\ Fairness

-----------------------------------------------------------------------------
(* Cross-Layer Invariants, Action Properties & Liveness                    *)
-----------------------------------------------------------------------------

TypeOK ==
    /\ oldLayout \in {"none", "cold", "live"}
    /\ migrated \in BOOLEAN
    /\ placed \subseteq AllowedBuilds
    /\ tested \subseteq placed
    /\ cur \in {0} \cup AllowedBuilds
    /\ prev \in {0} \cup AllowedBuilds
    /\ hlock \in {"none", "ho", "sup"}
    /\ hstate.stage \in {"none", "switching", "draining", "starting", "rolling-back"}
    /\ supLock \in BOOLEAN
    /\ supPc \in {"idle", "check", "watch", "reap", "rollback"}
    /\ supDisk.shunned \subseteq AllowedBuilds
    /\ srv.state \in {"down", "serving", "draining", "exited"}
    /\ maxReader \in {0, 3, 5}
    /\ sessions \in Nat

\* 1. Action property: while an old pre-StoreLock server answers on ~/.nexus/server.sock, no step migrates, switches or
\* starts a new server until the old server stops first.
LiveOldUntouched ==
    [][(oldLayout = "live") => (~migrated' /\ srv'.state = "down" /\ hstate'.stage = "none")]_vars

\* 2. No server ever serves or drains while unmigrated old-layout state remains in ~/.nexus.
OldMigratedBeforeServe ==
    srv.state \in {"serving", "draining"} => (oldLayout = "none" /\ migrated)

\* 3. The active `cur` build and any serving/draining build always carry passing test evidence.
CurrentAlwaysTested ==
    /\ (cur # 0) => (cur \in tested)
    /\ (srv.state \in {"serving", "draining"}) => (srv.b \in tested /\ srv.b \notin StartupBroken)

\* 4. Handover CLI and supervisor rollback serialize strictly on `<dir>/handover`'s OS flock.
HandoverLockSerialized ==
    (supPc = "rollback") => (hlock = "sup" /\ hstate.stage = "none")

\* 5. Neither `cur` nor a running server is ever older than the store's actual reader floor on disk.
NeverIncompatibleCurrent ==
    /\ (cur # 0) => (maxReader <= Writes(cur))
    /\ (srv.state \in {"serving", "draining"}) => (maxReader <= Writes(srv.b))

\* 6. Migrated and committed store sessions never vanish across upgrades, drains, crashes or rollbacks.
SessionsNeverLost ==
    (migrated /\ InitOldLayout # "none") => (sessions >= 1)

\* 7. Action property: an open turn on a non-crashing build is never cut by handover or supervision.
OpenTurnPreserved ==
    [][(srv.state \in {"serving", "draining"} /\ srv.turn /\ srv.b \notin LateBroken /\
        supLock' = supLock) =>
       (srv'.turn \/ sessions' > sessions)]_vars

\* 8. Action property: intentional `stop` (exit 0) marks supervision stopped and stays stopped unless an explicit
\* handover clears it.
StopHonored ==
    [][
        /\ ((srv.state = "exited" /\ srv.code = ExitStop /\ srv'.state = "down" /\ supDeaths' = supDeaths) =>
            supDisk'.stopped)
        /\ ((supDisk.stopped /\ ~supLock /\ hlock = "none" /\ hlock' = "none") => ~supLock')
    ]_vars

\* 9. Action property: reaping a `drain` exit (75) never marks supervision stopped.
DrainNeverStops ==
    [][(srv.state = "exited" /\ srv.code = ExitDrain /\ srv'.state = "down" /\ supDeaths' = supDeaths) =>
       ~supDisk'.stopped]_vars

\* 10. Liveness: from any cut, crash, old-layout state or broken upgrade, the system settles cleanly (serving a healthy
\* build, stopped by the user, idle before first selection, or halted cleanly when all compatible builds are exhausted).
SettlesOrHalts == <>Settled

\* 11. Liveness: whenever no breaking format-5 write occurs, no user stop occurs, and at least one slot of `(cur, prev)`
\* holds a healthy tested build, recovery eventually reaches and keeps a healthy serving build (never falsely halting
\* because an old server was live or because version-only `formatsRefusal` blocked rolling back from a crashed 5/5 build
\* that never wrote format 5!).
HealthyBuild(b) == b # 0 /\ b \in tested /\ b \notin (StartupBroken \cup LateBroken)

WorkingFallbackServes ==
    ([]~wroteBreaking /\ [](stops = 0) /\
     [](HealthyBuild(cur) \/ HealthyBuild(prev))) =>
    <>[](srv.state = "serving" /\ HealthyBuild(srv.b))

=============================================================================
