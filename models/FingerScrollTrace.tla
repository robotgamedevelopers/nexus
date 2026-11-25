-------------------------- MODULE FingerScrollTrace --------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real                 *)
(* nexus::FingerScroll (src/finger_scroll.h) is a behaviour of             *)
(* FingerScroll.tla.                                                       *)
(*                                                                         *)
(* tests/voice/finger_walk.cpp drives the code with a seeded random walk   *)
(* of presses, moves, ups, cancels, changes of range and frames, its times *)
(* its own numbers, and writes FingerScrollTraceData.tla beside this       *)
(* module: the list's constants in rows (TraceMaxes, TraceBand,            *)
(* TraceBound) and Trace, one record per step: the operation (and a        *)
(* range's rows), and the code's state after it quantised to the model's   *)
(* (the phase; the offset in whole rows rounded away from 0, so any part   *)
(* of a row past an end is past it; the range; caught; a fling's way; a    *)
(* spring's end, side and stage, out while its speed is away from its end; *)
(* what an up returned).                                                   *)
(*                                                                         *)
(* Each step pins the spec's next state to the record and takes the        *)
(* FingerScroll action the record names. A step the spec cannot take to    *)
(* that state has no successor, which TLC reports as a deadlock. The       *)
(* invariants and action properties are checked on the trace as well.      *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, FingerScrollTraceData

VARIABLES phase, off, max, caught, dir, send, side, stage, tap, act, inputs, l

F == INSTANCE FingerScroll WITH Maxes <- TraceMaxes, Band <- TraceBand, Bound <- TraceBound, Inputs <- Len(Trace)

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

\* The next state is the code's.
Pin(s) ==
    /\ phase' = s.phase /\ off' = s.off /\ max' = s.max /\ caught' = s.caught
    /\ dir' = s.dir /\ send' = s.send /\ side' = s.side /\ stage' = s.stage /\ tap' = s.tap

\* The FingerScroll action a record names.
Step(s) ==
    CASE s.op = "down"   -> F!Down
      [] s.op = "move"   -> F!Move
      [] s.op = "up"     -> F!Up
      [] s.op = "cancel" -> F!Cancel
      [] s.op = "range"  -> s.m \in TraceMaxes /\ F!SetRange(s.m)
      [] s.op = "tick"   -> F!Tick
      [] OTHER           -> FALSE

traceVars == <<phase, off, max, caught, dir, send, side, stage, tap, act, inputs, l>>

TraceInit == l = 1 /\ F!Init /\ off = Trace[1].off /\ max = Trace[1].max

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Pin(Trace[l + 1])
    /\ Step(Trace[l + 1])

\* The end of the trace: stutter, so that a trace read to its end is no deadlock.
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == F!TypeOK
AtRestInside == F!AtRestInside
FlingInside == F!FlingInside
SpringNeverCrossesInside == F!SpringNeverCrossesInside
SpringToItsEnd == F!SpringToItsEnd
DragIsNoTap == F!DragIsNoTap
CaughtIsNoTap == F!CaughtIsNoTap
ShrinkPullsIn == F!ShrinkPullsIn
=============================================================================
