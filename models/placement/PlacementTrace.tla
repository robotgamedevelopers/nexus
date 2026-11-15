--------------------------- MODULE PlacementTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real placement code  *)
(* (src/server/builds.cpp, every step through Options::hook) on real files *)
(* is a behaviour of Placement.tla with no bug.                            *)
(*                                                                         *)
(* tests/server/builds_walk.cpp writes PlacementTraceData.tla beside this  *)
(* module: the instance (TInitial, TOthers, TTested, TFolders) and Trace,  *)
(* one record per step of the walk: op (the step taken: a Placement       *)
(* action's name in the code's words), the process p (an actor a1, a2 or   *)
(* the server s1), a start's verb, tree and evidence (v, t, e), and after  *)
(* the step what the disk and the processes show: each actor's next step   *)
(* (pcs), who holds the builds lock (asked of the system: lock), the       *)
(* current and previous builds through builds/active (cur, prev: a tree,   *)
(* 0 none), the trees named in the builds directory (named), those whose   *)
(* folder is whole (whole) and tested (marks), and the server's state.     *)
(*                                                                         *)
(* Each step takes the action its op names, and the state it reaches must  *)
(* be the code's: a step the spec cannot take, or takes to another state,  *)
(* has no successor. The trace is accepted when a behaviour reaches its    *)
(* last record (TraceUnfinished violated), and rejected when none does.    *)
(* Every Placement invariant is checked on every state on the way.         *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC, PlacementTraceData

VARIABLES pc, verb, tgt, ev, wf, np, frm, ph, cands, lock, pex, fl, fd, dir, dirD, act, actD, pend, sealed, srv,
          nextF, ops, crashes, deaths, serves, l

Unbounded == 1000000

S == INSTANCE Placement WITH Actors <- {"a1", "a2"}, Servers <- {"s1"}, Trees <- {1, 2, 3}, Tested <- TTested,
                             Initial <- TInitial, Others <- TOthers, MaxOps <- Unbounded, MaxCrashes <- Unbounded,
                             MaxDeaths <- Unbounded, MaxServes <- Unbounded, MaxFolders <- TFolders, Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ <<pc["a1"], pc["a2"]>> = r.pcs
    /\ lock = r.lock
    /\ act.pn.x = r.cur
    /\ act.pn.y = r.prev
    /\ {t \in 1..3 : S!B(t) \in DOMAIN dir} = r.named
    /\ {t \in 1..3 : S!BuildOK(t)} = r.whole
    /\ {t \in 1..3 : S!B(t) \in DOMAIN dir /\ fl[dir[S!B(t)]].mark} = r.marks
    /\ Cardinality({n \in DOMAIN dir : S!Leftover(n)}) = r.left
    /\ srv["s1"].st = r.srv

TraceMatches == Matches(l)

\* The spec chooses where the code's record does not say (which subset of a crash's unsynced changes survived, which
\* candidate a prune takes first), so a behaviour that chose otherwise ends early: that is no failure. The trace is a
\* behaviour of the spec when some behaviour reaches its last record: TLC reports this invariant violated. When none
\* does, TLC ends with no error, and the trace is rejected.
TraceUnfinished == l < Len(Trace)

Step(r) ==
    CASE r.op = "start" -> S!Start(r.p, r.v, r.t, r.e)
      [] r.op = "lock" -> S!LockTry(r.p)
      [] r.op = "settle" -> S!Settle(r.p)
      [] r.op = "clean" -> S!CleanOne(r.p)
      [] r.op = "mktmp" -> S!MkTmp(r.p)
      [] r.op = "write" -> S!Write(r.p)
      [] r.op = "manifest" -> S!Manifest(r.p)
      [] r.op = "sync" -> S!Sync(r.p)
      [] r.op = "verify" -> S!Verify(r.p)
      [] r.op = "rename" -> S!Rename(r.p)
      [] r.op = "syncdir" -> S!DirSync(r.p, "syncdir", "unlock")
      [] r.op = "drop" -> S!Drop(r.p)
      [] r.op = "unlock" -> S!Unlock(r.p)
      [] r.op = "check" -> S!Check(r.p)
      [] r.op = "pmk" -> S!PMk(r.p)
      [] r.op = "psync" -> S!PSync(r.p)
      [] r.op = "prename" -> S!PRename(r.p)
      [] r.op = "pdsync" -> S!DirSync(r.p, "pdsync", "anew")
      [] r.op = "anew" -> S!ANew(r.p)
      [] r.op = "arename" -> S!ARename(r.p)
      [] r.op = "async" -> S!ASync(r.p)
      [] r.op = "plist" -> S!PList(r.p)
      [] r.op = "ptry" -> S!PTry(r.p)
      [] r.op = "pgone" -> S!PGone(r.p)
      [] r.op = "pgsync" -> S!PGSync(r.p)
      [] r.op = "premove" -> S!PRemove(r.p)
      [] r.op = "resolve" -> S!SResolve(r.p) /\ srv'[r.p].n = S!B(r.t)
      [] r.op = "hold" -> S!SHold(r.p)
      [] r.op = "stop" -> S!SStop(r.p)
      [] r.op = "die" -> S!Die(r.p)
      [] r.op = "crash" -> S!Crash
      [] OTHER -> FALSE

traceVars == <<pc, verb, tgt, ev, wf, np, frm, ph, cands, lock, pex, fl, fd, dir, dirD, act, actD, pend, sealed, srv,
               nextF, ops, crashes, deaths, serves, l>>

TraceInit == l = 1 /\ S!Init /\ TraceMatches

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])
    /\ TraceMatches'

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

CurrentValid == S!CurrentValid
PreviousValid == S!PreviousValid
PointerResolves == S!PointerResolves
PrevIsLastCurrent == S!PrevIsLastCurrent
NamedWhole == S!NamedWhole
SealedUnchanged == S!SealedUnchanged
InUseKept == S!InUseKept
OneSwitcher == S!OneSwitcher
LockHeld == S!LockHeld
=============================================================================
