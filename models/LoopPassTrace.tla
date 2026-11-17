--------------------------- MODULE LoopPassTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real server pass     *)
(* (src/server/pass.h: PassOrder and take_pass_step, the dispatch          *)
(* Server::run runs each pass through; DurableStore over Store, Journal    *)
(* and MemFs; Hub; Drivers and Supervisor; Applier and FakeMosh links) is  *)
(* a behaviour of LoopPass.tla.                                            *)
(*                                                                         *)
(* tests/server/pass_walk.cpp writes LoopPassTraceData.tla beside this     *)
(* module. Each record of Trace names the step taken (op) and records the  *)
(* code's state after it: PassOrder's step index, failed, Store::batching, *)
(* Drivers::holding, PassOrder::changed, each window's actions, received,  *)
(* Applier and acknowledged counters, the live DurableStore state (mem),   *)
(* the durable state recovered from MemFs with all volatile writes dropped *)
(* (disk), the journal's unflushed record count (batchLen), the            *)
(* supervisor's phase, the Send and Stop lines actually written to drivers *)
(* (decoded by driver::ServerReader), the state staged on links (linkSnap) *)
(* and the state transmitted by Tick (sentSnap).                           *)
(*                                                                         *)
(* Each step takes the LoopPass action its op names and pins the next      *)
(* state to the record (Matches). Any step the spec cannot take, or takes  *)
(* to a different state (release-before-flush, state-before-flush,         *)
(* tick-before-flush, no-hold, release-on-fail), has no successor and TLC  *)
(* reports a deadlock. Every LoopPass safety property is checked too.      *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC, LoopPassTraceData

VARIABLES stepIdx, failed, batching, holding, changed, acts, rcvd, applier, acked,
          mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
          linkSnap, sentSnap, crashes, fails, l

L == INSTANCE LoopPass WITH Clients <- TraceClients,
                            MaxActs <- TraceMaxActs,
                            Kinds <- {"Send", "Queue", "Stop"},
                            MaxCrashes <- TraceMaxCrashes,
                            MaxFails <- TraceMaxFails

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Ord == L!DefaultOrder

StoreView(st) ==
    [tr      |-> st.tr,
     queue   |-> st.queue,
     waiting |-> st.waiting,
     open    |-> st.open,
     next    |-> st.next,
     applied |-> st.applied]

StopTurns(stops) == [i \in 1..Len(stops) |-> stops[i].turn]

Matches(k) ==
    LET r == Trace[k] IN
    /\ stepIdx = r.stepIdx
    /\ failed = r.failed
    /\ batching = r.batching
    /\ holding = r.holding
    /\ changed = r.changed
    /\ acts = r.acts
    /\ rcvd = r.rcvd
    /\ applier = r.applier
    /\ acked = r.acked
    /\ StoreView(mem) = r.mem
    /\ StoreView(disk) = r.disk
    /\ Len(batch) = r.batchLen
    /\ drvPhase = r.drvPhase
    /\ drvSends = r.drvSends
    /\ StopTurns(drvStops) = r.drvStops
    /\ linkSnap = r.linkSnap
    /\ sentSnap = r.sentSnap

TraceMatches == Matches(l)

Step(r) ==
    CASE r.op = "act"          -> L!Act(r.c, r.kind)
      [] r.op = "tick"         -> L!Tick(Ord)
      [] r.op = "hold"         -> L!Hold(Ord, TRUE)
      [] r.op = "driver_hello" -> L!DriverHello(Ord)
      [] r.op = "driver_done"  -> L!DriverDone(Ord)
      [] r.op = "recv"         -> L!RecvOnly(Ord, r.c, r.to)
      [] r.op = "apply"        -> L!ApplyAct(Ord, r.c, r.to)
      [] r.op = "applied"      -> L!Applied(Ord)
      [] r.op = "apply_fail"   -> L!ApplyFail(Ord, r.c, r.to)
      [] r.op = "flush"        -> L!FlushOk(Ord)
      [] r.op = "flush_fail"   -> L!FlushFail(Ord, "stop")
      [] r.op = "release"      -> L!Release(Ord)
      [] r.op = "state"        -> L!Publish(Ord)
      [] r.op = "crash"        -> L!Crash(r.kept, FALSE)
      [] OTHER                 -> FALSE

traceVars == <<stepIdx, failed, batching, holding, changed, acts, rcvd, applier, acked,
               mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
               linkSnap, sentSnap, crashes, fails, l>>

TraceInit == l = 1 /\ L!Init /\ TraceMatches

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])
    /\ TraceMatches'

\* The end of the trace: stutter, so that a trace read to its end is no deadlock.
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == L!TypeOK
ExactlyOnce == L!ExactlyOnce
PromptAtMostOnce == L!PromptAtMostOnce
TurnsIncrease == L!TurnsIncrease
DriverOnlyDurable == L!DriverOnlyDurable
LinkStateDurable == L!LinkStateDurable
AckedOnlyDurable == L!AckedOnlyDurable
DurableMonotonic == L!DurableMonotonic
NoOutBeforeFlush == L!NoOutBeforeFlush
=============================================================================
