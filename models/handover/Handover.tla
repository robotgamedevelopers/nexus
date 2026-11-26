------------------------------ MODULE Handover ------------------------------
(***************************************************************************)
(* Handover: a placed build replaces the running server under an OS lock,  *)
(* with durable step-by-step state on disk so a handover cut anywhere      *)
(* resumes from what is on disk (src/server/handover.h, docs/builds.md;    *)
(* docs/roadmap.sexp `takeover-handover`, layer 3 of 6).                   *)
(*                                                                         *)
(* Layer 1 (StoreLock.tla) ensures at most one server holds the store;     *)
(* Layer 2 (Placement.tla) ensures placed builds are immutable, verified,  *)
(* and switched atomically as a (current, previous) pair. Layer 3 sits     *)
(* cleanly on top of both:                                                 *)
(*   - One handover runs at a time, enforced by an exclusive OS flock on   *)
(*     `<server dir>/handover` (store/fs.h: unlockable_filesystem and      *)
(*     locks_kept checked, released by the OS however the process ends).   *)
(*   - Before switching `builds/active` or touching the running server,    *)
(*     the handover writes `<server dir>/handover/state` through a synced  *)
(*     temporary file (`.state-*`), renames it over `state`, and syncs the *)
(*     `handover/` directory: target, previous (the last serving build),   *)
(*     and stage (`switching`, `draining`, `starting`, `rolling-back`).    *)
(*   - Any actor acquiring the handover lock settles `handover/`, removes  *)
(*     `.state-*` leftovers, and if `handover/state` is present, finishes  *)
(*     resuming that interrupted handover before starting any new one, so  *)
(*     `previous` (the last known good build) is never overwritten by a    *)
(*     broken build that never served.                                     *)
(*   - A server that knows `drain` finishes its open turn first (up to its *)
(*     drain deadline) before exiting; a server from before `drain`        *)
(*     (`LegacyBuild`) is stopped with `stop`.                             *)
(*   - If the target build fails to serve in time, handover records        *)
(*     `rolling-back`, switches `builds/active` back to `previous`, starts *)
(*     `previous` until it serves, and only then removes `handover/state`. *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Actors,       \* processes running `handover <build>` or `handover resume`
    Builds,       \* builds placed under builds/ (e.g. {1, 2, 3})
    Tested,       \* subset of Builds whose test evidence passed
    Broken,       \* subset of Builds that fail to serve when started
    Initial,      \* build current at the start (in Tested \ Broken)
    StartSrv,     \* build serving at the start: Initial, LegacyBuild, or 0
    MaxOps,       \* commands each actor starts
    MaxCrashes,   \* machine power-cut crashes
    MaxDeaths,    \* actors killed (SIGKILL)
    MaxTurns,     \* AI turns opened on the running server
    Bug           \* "none", or one reversed witness's break

LegacyBuild == 9
None == "none"
Fids == 1..5

ASSUME Builds \subseteq 1..3 /\ Tested \subseteq Builds /\ Broken \subseteq Builds
ASSUME Initial \in (Tested \ Broken)
ASSUME StartSrv \in {0, Initial, LegacyBuild}
ASSUME Bug \in {"none", "no_lock", "no_sync", "in_memory_only", "overwrite_prev",
                "no_rollback", "stop_cuts", "no_drain_deadline", "allow_untested", "serve_broken"}

Stages == {"switching", "draining", "starting", "rolling-back"}
Verbs == {"to", "resume"}

FreeFile == [kind |-> "free", target |-> 0, prev |-> 0, stage |-> "switching"]
DownSrv == [b |-> 0, draining |-> FALSE, turn |-> FALSE, late |-> FALSE]

\* Names in `<server dir>/handover`: SName is `state`, TName(f) is `.state-<f>`.
SName == [k |-> "s", f |-> 0]
TName(f) == [k |-> "t", f |-> f]
NoName == [k |-> "-", f |-> 0]

VARIABLES
    pc,       \* each actor's next step (matches Options::hook in C++)
    verb,     \* "to" or "resume"
    tgt,      \* requested build for "to" (0 for "resume")
    rtgt,     \* active handover's target build
    rprev,    \* active handover's rollback build (0 when none)
    didOwn,   \* true once this actor's own requested target is underway or done
    wf,       \* file id in handover/ for an in-flight state write
    lock,     \* holder of handover/'s exclusive flock, or None
    cur,      \* builds/active current build
    prev,     \* builds/active previous build (0 when none)
    hf,       \* handover/ files now
    hfd,      \* handover/ files as last fsync'd
    hdir,     \* handover/ directory entries now: name -> file id
    hdirD,    \* handover/ directory entries as last fsync'd
    hpend,    \* pending directory operations since last sync_dir("handover")
    srv,      \* running server: [b, draining, turn, late]
    ops, crashes, deaths, turns

vars == <<pc, verb, tgt, rtgt, rprev, didOwn, wf, lock, cur, prev,
          hf, hfd, hdir, hdirD, hpend, srv, ops, crashes, deaths, turns>>

Remove(d, n) == [m \in DOMAIN d \ {n} |-> d[m]]
Add(d, n, f) == (n :> f) @@ d

Op(from, to, f) == [from |-> from, to |-> to, f |-> f]
ApplyD(d, o) == LET r == IF o.from # NoName THEN Remove(d, o.from) ELSE d
                IN IF o.to # NoName THEN Add(r, o.to, o.f) ELSE r

RECURSIVE Replay(_, _)
Replay(d, s) == IF s = <<>> THEN d ELSE Replay(ApplyD(d, Head(s)), Tail(s))

RECURSIVE Pick(_, _, _)
Pick(s, keep, i) ==
    IF i > Len(s) THEN <<>>
    ELSE (IF i \in keep THEN <<s[i]>> ELSE <<>>) \o Pick(s, keep, i + 1)

ChangeH(o) ==
    /\ hdir' = ApplyD(hdir, o)
    /\ hpend' = Append(hpend, o)

\* Syncing `handover/` commits pending directory ops and reclaims unreferenced inodes.
SyncH ==
    /\ hdirD' = hdir
    /\ hpend' = <<>>
    /\ LET live == {hdir[n] : n \in DOMAIN hdir} IN
       /\ hf' = [x \in Fids |-> IF x \in live THEN hf[x] ELSE FreeFile]
       /\ hfd' = [x \in Fids |-> IF x \in live THEN hfd[x] ELSE FreeFile]

UsedFids ==
    {hdir[n] : n \in DOMAIN hdir}
    \cup {hdirD[n] : n \in DOMAIN hdirD}
    \cup {hpend[i].f : i \in 1..Len(hpend)}

FreeFids == Fids \ UsedFids
PickFreeFid == CHOOSE f \in FreeFids : \A g \in FreeFids : f <= g

HasLeftover == \E n \in DOMAIN hdir : n.k = "t"

HasState == SName \in DOMAIN hdir
StateRec == hf[hdir[SName]]

Init ==
    /\ pc = [a \in Actors |-> "idle"]
    /\ verb = [a \in Actors |-> "resume"]
    /\ tgt = [a \in Actors |-> 0]
    /\ rtgt = [a \in Actors |-> 0]
    /\ rprev = [a \in Actors |-> 0]
    /\ didOwn = [a \in Actors |-> FALSE]
    /\ wf = [a \in Actors |-> 0]
    /\ lock = None
    /\ cur = Initial
    /\ prev = 0
    /\ hf = [f \in Fids |-> FreeFile]
    /\ hfd = hf
    /\ hdir = [n \in {} |-> 0]
    /\ hdirD = hdir
    /\ hpend = <<>>
    /\ srv = IF StartSrv = 0 THEN DownSrv
             ELSE [b |-> StartSrv, draining |-> FALSE, turn |-> FALSE, late |-> FALSE]
    /\ ops = [a \in Actors |-> 0]
    /\ crashes = 0
    /\ deaths = 0
    /\ turns = 0

ResetLocals(a) ==
    /\ verb' = [verb EXCEPT ![a] = "resume"]
    /\ tgt' = [tgt EXCEPT ![a] = 0]
    /\ rtgt' = [rtgt EXCEPT ![a] = 0]
    /\ rprev' = [rprev EXCEPT ![a] = 0]
    /\ didOwn' = [didOwn EXCEPT ![a] = FALSE]
    /\ wf' = [wf EXCEPT ![a] = 0]

ResetAll ==
    /\ verb' = [a \in Actors |-> "resume"]
    /\ tgt' = [a \in Actors |-> 0]
    /\ rtgt' = [a \in Actors |-> 0]
    /\ rprev' = [a \in Actors |-> 0]
    /\ didOwn' = [a \in Actors |-> FALSE]
    /\ wf' = [a \in Actors |-> 0]

HDisk == <<hf, hfd, hdir, hdirD, hpend>>
Locals == <<verb, tgt, rtgt, rprev, didOwn, wf>>
Counts == <<ops, crashes, deaths, turns>>

\* Start a handover command: `handover <t>` (v = "to") or `handover resume` (v = "resume", t = 0).
Start(a, v, t) ==
    /\ pc[a] = "idle" /\ ops[a] < MaxOps
    /\ v = "to" => t \in Builds
    /\ v = "resume" => t = 0
    /\ pc' = [pc EXCEPT ![a] = "lock"]
    /\ verb' = [verb EXCEPT ![a] = v]
    /\ tgt' = [tgt EXCEPT ![a] = t]
    /\ rtgt' = [rtgt EXCEPT ![a] = 0]
    /\ rprev' = [rprev EXCEPT ![a] = 0]
    /\ didOwn' = [didOwn EXCEPT ![a] = FALSE]
    /\ wf' = [wf EXCEPT ![a] = 0]
    /\ ops' = [ops EXCEPT ![a] = @ + 1]
    /\ UNCHANGED <<lock, cur, prev, HDisk, srv, crashes, deaths, turns>>

\* Acquire exclusive flock on `<server dir>/handover`.
LockTry(a) ==
    /\ pc[a] = "lock"
    /\ IF Bug = "no_lock"
       THEN /\ pc' = [pc EXCEPT ![a] = "settle"]
            /\ UNCHANGED lock
       ELSE IF lock = None
       THEN /\ lock' = a
            /\ pc' = [pc EXCEPT ![a] = "settle"]
       ELSE UNCHANGED <<pc, lock>>
    /\ UNCHANGED <<Locals, cur, prev, HDisk, srv, Counts>>

LockGiveUp(a) ==
    /\ pc[a] = "lock" /\ lock # None /\ lock # a
    /\ pc' = [pc EXCEPT ![a] = "idle"]
    /\ ResetLocals(a)
    /\ UNCHANGED <<lock, cur, prev, HDisk, srv, Counts>>

\* Settle `handover/` by syncing the directory before judging leftovers or reading `state`.
Settle(a) ==
    /\ pc[a] = "settle"
    /\ SyncH
    /\ pc' = [pc EXCEPT ![a] = IF HasLeftover THEN "clean" ELSE "inspect"]
    /\ UNCHANGED <<Locals, lock, cur, prev, hdir, srv, Counts>>

\* Remove `.state-*` leftovers left by an interrupted write.
CleanOne(a) ==
    /\ pc[a] = "clean"
    /\ IF HasLeftover
       THEN \E n \in DOMAIN hdir :
              /\ n.k = "t"
              /\ ChangeH(Op(n, NoName, 0))
              /\ pc' = [pc EXCEPT ![a] = IF \E m \in DOMAIN hdir : m.k = "t" /\ m # n THEN "clean" ELSE "inspect"]
       ELSE /\ pc' = [pc EXCEPT ![a] = "inspect"]
            /\ UNCHANGED <<hdir, hpend>>
    /\ UNCHANGED <<Locals, lock, cur, prev, hf, hfd, hdirD, srv, Counts>>

StagePc(st) ==
    CASE st = "switching"    -> "switch"
      [] st = "draining"     -> "drain"
      [] st = "starting"     -> "start"
      [] st = "rolling-back" -> "rollback"

\* Inspect `handover/state`, `builds/active`, and the running server.
\* If an unfinished `handover/state` is on disk, resume it first (unless `overwrite_prev` skips it on "to").
Inspect(a) ==
    /\ pc[a] = "inspect"
    /\ IF HasState /\ ~(Bug = "overwrite_prev" /\ verb[a] = "to")
       THEN IF StateRec.kind # "rec"
            THEN /\ pc' = [pc EXCEPT ![a] = "unlock"]
                 /\ UNCHANGED <<rtgt, rprev, didOwn>>
            ELSE /\ rtgt' = [rtgt EXCEPT ![a] = StateRec.target]
                 /\ rprev' = [rprev EXCEPT ![a] = StateRec.prev]
                 /\ didOwn' = [didOwn EXCEPT ![a] = (verb[a] = "resume" \/ tgt[a] = StateRec.target)]
                 /\ pc' = [pc EXCEPT ![a] = StagePc(StateRec.stage)]
       ELSE IF verb[a] = "resume"
       THEN IF srv.b = cur /\ ~srv.draining
            THEN /\ pc' = [pc EXCEPT ![a] = "unlock"]
                 /\ didOwn' = [didOwn EXCEPT ![a] = TRUE]
                 /\ UNCHANGED <<rtgt, rprev>>
            ELSE /\ rtgt' = [rtgt EXCEPT ![a] = cur]
                 /\ rprev' = [rprev EXCEPT ![a] = 0]
                 /\ didOwn' = [didOwn EXCEPT ![a] = TRUE]
                 /\ pc' = [pc EXCEPT ![a] = IF srv.b # 0 THEN "wdrain" ELSE "wstart"]
       ELSE \* verb[a] = "to"
            LET want == tgt[a]
                valid == want \in Builds /\ (want \in Tested \/ Bug = "allow_untested")
                already == (cur = want /\ srv.b = want /\ ~srv.draining)
                prevChoice == IF cur # want THEN cur ELSE 0
            IN IF ~valid \/ already
               THEN /\ pc' = [pc EXCEPT ![a] = "unlock"]
                    /\ didOwn' = [didOwn EXCEPT ![a] = TRUE]
                    /\ UNCHANGED <<rtgt, rprev>>
               ELSE /\ rtgt' = [rtgt EXCEPT ![a] = want]
                    /\ rprev' = [rprev EXCEPT ![a] = prevChoice]
                    /\ didOwn' = [didOwn EXCEPT ![a] = TRUE]
                    /\ pc' = [pc EXCEPT ![a] = "wswitch"]
    /\ UNCHANGED <<verb, tgt, wf, lock, cur, prev, HDisk, srv, Counts>>

\* Write `.state-*` (synced unless `no_sync`) and rename it over `handover/state`; `in_memory_only` writes nothing.
\* Old inodes remain intact until `SyncH` commits the directory, matching POSIX rename semantics.
WriteState(a, fromPc, st, syncPc, nextPc) ==
    /\ pc[a] = fromPc
    /\ IF Bug = "in_memory_only"
       THEN /\ pc' = [pc EXCEPT ![a] = nextPc]
            /\ UNCHANGED <<wf, HDisk>>
       ELSE /\ FreeFids # {}
            /\ LET f == PickFreeFid
                   rec == [kind |-> "rec", target |-> rtgt[a], prev |-> rprev[a], stage |-> st]
               IN /\ hf' = [hf EXCEPT ![f] = rec]
                  /\ hfd' = IF Bug = "no_sync" THEN hfd ELSE [hfd EXCEPT ![f] = rec]
                  /\ hdir' = Add(hdir, SName, f)
                  /\ hpend' = Append(hpend, Op(NoName, SName, f))
                  /\ wf' = [wf EXCEPT ![a] = f]
                  /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN nextPc ELSE syncPc]
                  /\ UNCHANGED hdirD
    /\ UNCHANGED <<verb, tgt, rtgt, rprev, didOwn, lock, cur, prev, srv, Counts>>

SyncState(a, fromPc, nextPc) ==
    /\ pc[a] = fromPc
    /\ SyncH
    /\ pc' = [pc EXCEPT ![a] = nextPc]
    /\ UNCHANGED <<Locals, lock, cur, prev, hdir, srv, Counts>>

\* Switch `builds/active` to `rtgt[a]` (idempotent when `cur = rtgt[a]`, matching `Builds::makeCurrent`).
Switch(a) ==
    /\ pc[a] = "switch"
    /\ IF cur # rtgt[a]
       THEN /\ prev' = cur
            /\ cur' = rtgt[a]
       ELSE UNCHANGED <<cur, prev>>
    /\ pc' = [pc EXCEPT ![a] = "wdrain"]
    /\ UNCHANGED <<Locals, lock, HDisk, srv, Counts>>

\* Drain (or, for LegacyBuild, stop) any running server not already serving `rtgt[a]`.
DrainStep(a) ==
    /\ pc[a] = "drain"
    /\ IF srv.b = 0
       THEN /\ pc' = [pc EXCEPT ![a] = "wstart"]
            /\ UNCHANGED srv
       ELSE IF srv.b = rtgt[a] /\ ~srv.draining
       THEN /\ pc' = [pc EXCEPT ![a] = "clear"]
            /\ UNCHANGED srv
       ELSE IF srv.b = LegacyBuild \/ Bug = "stop_cuts"
       THEN /\ srv' = DownSrv
            /\ pc' = [pc EXCEPT ![a] = "wstart"]
       ELSE IF ~srv.draining
       THEN /\ srv' = [srv EXCEPT !.draining = TRUE]
            /\ UNCHANGED pc
       ELSE UNCHANGED <<pc, srv>>
    /\ UNCHANGED <<Locals, lock, cur, prev, HDisk, Counts>>

\* Start `rtgt[a]` (through `current/bin/nexus-server serve`) and wait for it to answer SERVING.
StartStep(a) ==
    /\ pc[a] = "start"
    /\ IF srv.b = rtgt[a] /\ ~srv.draining
       THEN /\ pc' = [pc EXCEPT ![a] = "clear"]
            /\ UNCHANGED srv
       ELSE IF srv.b # 0
       THEN /\ pc' = [pc EXCEPT ![a] = "wdrain"]
            /\ UNCHANGED srv
       ELSE IF rtgt[a] \notin Broken \/ Bug = "serve_broken"
       THEN /\ srv' = [b |-> rtgt[a], draining |-> FALSE, turn |-> FALSE, late |-> FALSE]
            /\ UNCHANGED pc
       ELSE /\ UNCHANGED srv
            /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_rollback" \/ rprev[a] = 0 THEN "clear" ELSE "wrollback"]
    /\ UNCHANGED <<Locals, lock, cur, prev, HDisk, Counts>>

\* Switch `builds/active` back to `rprev[a]` (idempotent via `Builds::makeCurrent(rprev[a])`).
RollbackSwitch(a) ==
    /\ pc[a] = "rollback"
    /\ srv' = IF srv.b # 0 /\ srv.b # rprev[a] THEN DownSrv ELSE srv
    /\ IF cur # rprev[a]
       THEN /\ prev' = cur
            /\ cur' = rprev[a]
       ELSE UNCHANGED <<cur, prev>>
    /\ pc' = [pc EXCEPT ![a] = "start_rb"]
    /\ UNCHANGED <<Locals, lock, HDisk, Counts>>

\* Start `rprev[a]` and wait for it to answer SERVING.
StartRollback(a) ==
    /\ pc[a] = "start_rb"
    /\ IF srv.b = rprev[a] /\ ~srv.draining
       THEN /\ pc' = [pc EXCEPT ![a] = "clear"]
            /\ UNCHANGED srv
       ELSE IF srv.b = 0 /\ rprev[a] \notin Broken
       THEN /\ srv' = [b |-> rprev[a], draining |-> FALSE, turn |-> FALSE, late |-> FALSE]
            /\ UNCHANGED pc
       ELSE /\ pc' = [pc EXCEPT ![a] = "unlock"]
            /\ UNCHANGED srv
    /\ UNCHANGED <<Locals, lock, cur, prev, HDisk, Counts>>

\* Remove `handover/state`, then sync `handover/`.
ClearState(a) ==
    /\ pc[a] = "clear"
    /\ IF HasState
       THEN ChangeH(Op(SName, NoName, 0))
       ELSE UNCHANGED <<hdir, hpend>>
    /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN (IF didOwn[a] THEN "unlock" ELSE "inspect") ELSE "sclear"]
    /\ UNCHANGED <<Locals, lock, cur, prev, hf, hfd, hdirD, srv, Counts>>

SyncClear(a) ==
    /\ pc[a] = "sclear"
    /\ SyncH
    /\ pc' = [pc EXCEPT ![a] = IF didOwn[a] THEN "unlock" ELSE "inspect"]
    /\ UNCHANGED <<Locals, lock, cur, prev, hdir, srv, Counts>>

Unlock(a) ==
    /\ pc[a] = "unlock"
    /\ lock' = IF lock = a THEN None ELSE lock
    /\ pc' = [pc EXCEPT ![a] = "idle"]
    /\ ResetLocals(a)
    /\ UNCHANGED <<cur, prev, HDisk, srv, Counts>>

\* Server and environment actions.
TurnOpen ==
    /\ srv.b # 0 /\ ~srv.turn /\ turns < MaxTurns
    /\ srv' = [srv EXCEPT !.turn = TRUE]
    /\ turns' = turns + 1
    /\ UNCHANGED <<pc, Locals, lock, cur, prev, HDisk, ops, crashes, deaths>>

TurnEnd ==
    /\ srv.turn
    /\ srv' = [srv EXCEPT !.turn = FALSE]
    /\ UNCHANGED <<pc, Locals, lock, cur, prev, HDisk, Counts>>

DrainLate ==
    /\ srv.b # 0 /\ srv.draining /\ ~srv.late
    /\ Bug # "no_drain_deadline"
    /\ srv' = [srv EXCEPT !.late = TRUE]
    /\ UNCHANGED <<pc, Locals, lock, cur, prev, HDisk, Counts>>

SrvDrainExit ==
    /\ srv.b # 0 /\ srv.draining /\ (~srv.turn \/ srv.late)
    /\ srv' = DownSrv
    /\ UNCHANGED <<pc, Locals, lock, cur, prev, HDisk, Counts>>

\* Actor killed (SIGKILL): disk stays as it is, OS lock released at once.
Die(a) ==
    /\ pc[a] # "idle" /\ deaths < MaxDeaths
    /\ pc' = [pc EXCEPT ![a] = "idle"]
    /\ lock' = IF lock = a THEN None ELSE lock
    /\ deaths' = deaths + 1
    /\ ResetLocals(a)
    /\ UNCHANGED <<cur, prev, HDisk, srv, ops, crashes, turns>>

\* Machine crash: all processes end (locks released, server down); unsynced file bytes revert or stay, and any ordered
\* subset of unsynced handover/ directory ops survives.
Crash ==
    /\ crashes < MaxCrashes
    /\ \E keep \in SUBSET (1..Len(hpend)), keepF \in SUBSET {f \in Fids : hf[f] # hfd[f]} :
        LET d == Replay(hdirD, Pick(hpend, keep, 1))
            live == {d[n] : n \in DOMAIN d}
            f == [x \in Fids |-> IF x \notin live THEN FreeFile
                                 ELSE IF x \in keepF THEN hf[x] ELSE hfd[x]]
        IN /\ hdir' = d /\ hdirD' = d
           /\ hpend' = <<>>
           /\ hf' = f /\ hfd' = f
    /\ pc' = [a \in Actors |-> "idle"]
    /\ lock' = None
    /\ srv' = DownSrv
    /\ crashes' = crashes + 1
    /\ ResetAll
    /\ UNCHANGED <<cur, prev, ops, deaths, turns>>

Step(a) ==
    \/ LockTry(a) \/ LockGiveUp(a) \/ Settle(a) \/ CleanOne(a) \/ Inspect(a)
    \/ WriteState(a, "wswitch", "switching", "sswitch", "switch")
    \/ SyncState(a, "sswitch", "switch")
    \/ Switch(a)
    \/ WriteState(a, "wdrain", "draining", "sdrain", "drain")
    \/ SyncState(a, "sdrain", "drain")
    \/ DrainStep(a)
    \/ WriteState(a, "wstart", "starting", "sstart", "start")
    \/ SyncState(a, "sstart", "start")
    \/ StartStep(a)
    \/ WriteState(a, "wrollback", "rolling-back", "srollback", "rollback")
    \/ SyncState(a, "srollback", "rollback")
    \/ RollbackSwitch(a) \/ StartRollback(a)
    \/ ClearState(a) \/ SyncClear(a) \/ Unlock(a)

Done ==
    /\ \A a \in Actors : pc[a] = "idle" /\ ops[a] = MaxOps
    /\ ~srv.draining
    /\ UNCHANGED vars

Next ==
    \/ \E a \in Actors : \/ \E v \in Verbs, t \in Builds \cup {0} : Start(a, v, t)
                         \/ Step(a)
                         \/ Die(a)
    \/ TurnOpen \/ TurnEnd \/ DrainLate \/ SrvDrainExit
    \/ Crash
    \/ Done

Spec == Init /\ [][Next]_vars

\* Fairness: an actor that has begun a command takes its steps; a draining server's deadline eventually passes and a
\* ready draining server exits. TurnEnd, Start, Die, and Crash are never assumed.
Live ==
    /\ Spec
    /\ (\A a \in Actors : WF_vars(Step(a)))
    /\ WF_vars(DrainLate)
    /\ WF_vars(SrvDrainExit)

\* Cut-and-resume spec: actor "a1" starts one handover (to any build, good or broken) and may be killed or crashed at
\* any step; actor "a2" runs `handover resume` after "a1" has ended and finishes unhindered.
A1Start == \E t \in Builds : ops["a1"] = 0 /\ Start("a1", "to", t)
ResumeTrigger == ops["a1"] = 1 /\ pc["a1"] = "idle" /\ ops["a2"] = 0 /\ Start("a2", "resume", 0)

CutResumeNext ==
    \/ A1Start
    \/ Step("a1") \/ Die("a1")
    \/ ResumeTrigger
    \/ Step("a2")
    \/ TurnOpen \/ TurnEnd \/ DrainLate \/ SrvDrainExit
    \/ (ops["a2"] = 0 /\ Crash)
    \/ Done

CutResumeLive ==
    /\ Init
    /\ [][CutResumeNext]_vars
    /\ WF_vars(A1Start)
    /\ WF_vars(Step("a1"))
    /\ WF_vars(Step("a2"))
    /\ WF_vars(DrainLate)
    /\ WF_vars(SrvDrainExit)
    /\ WF_vars(ResumeTrigger)

-----------------------------------------------------------------------------
\* Properties

AllPcs == {"idle", "lock", "settle", "clean", "inspect",
           "wswitch", "sswitch", "switch",
           "wdrain", "sdrain", "drain",
           "wstart", "sstart", "start",
           "wrollback", "srollback", "rollback", "start_rb",
           "clear", "sclear", "unlock"}

TypeOK ==
    /\ pc \in [Actors -> AllPcs]
    /\ lock \in Actors \cup {None}
    /\ cur \in Builds
    /\ prev \in Builds \cup {0}
    /\ srv.b \in Builds \cup {0, LegacyBuild}
    /\ srv.draining \in BOOLEAN /\ srv.turn \in BOOLEAN /\ srv.late \in BOOLEAN

\* At most one handover runs past the lock at a time, and its holder owns every step.
OneHandover ==
    /\ Cardinality({a \in Actors : pc[a] \notin {"idle", "lock"}}) <= 1
    /\ \A a \in Actors : pc[a] \notin {"idle", "lock"} => lock = a

\* `current` always names a tested build.
CurrentTested == cur \in Tested

\* A broken build never reaches SERVING.
NeverServesBroken == srv.b \notin Broken

\* Every durable `handover/state` record on disk is whole, names a tested target, and keeps a working rollback build.
StateValid ==
    HasState =>
        /\ StateRec.kind = "rec"
        /\ StateRec.target \in Tested
        /\ StateRec.prev \in (Tested \ Broken) \cup {0}
        /\ StateRec.stage \in Stages

\* Whenever no handover is running and no unfinished `handover/state` is on disk, `current` is a working build.
IdleCurrentWorks ==
    (~HasState /\ \A a \in Actors : pc[a] = "idle") => cur \in (Tested \ Broken)

\* A non-legacy server with an open turn never stops for a handover before its drain deadline (unless the machine crashes).
DrainCutsOnlyLate ==
    [][(srv.b # 0 /\ srv.b # LegacyBuild /\ srv'.b = 0 /\ srv.turn /\ crashes' = crashes) => srv.late]_vars

\* A running server that was not asked to drain stops only when it is a pre-drain legacy server (or the machine crashes).
StopOnlyLegacy ==
    [][(srv.b # 0 /\ ~srv.draining /\ srv'.b = 0 /\ crashes' = crashes) => srv.b = LegacyBuild]_vars

\* Every handover or resume begun finishes (done, rolled back, refused, or busy).
HandoverEnds == \A a \in Actors : (pc[a] # "idle") ~> (pc[a] = "idle")

\* Cut anywhere (SIGKILL or power loss, target working or broken, turn open or not), `handover resume` brings a
\* working tested build to SERVING, clears `handover/state`, and leaves `current` matching the serving build.
AlwaysRecoversToServing ==
    <>[](srv.b \in (Tested \ Broken) /\ ~srv.draining /\ cur = srv.b /\ ~HasState /\ \A a \in Actors : pc[a] = "idle")

=============================================================================
