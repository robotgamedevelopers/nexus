--------------------------- MODULE ScrollHoldTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real hold            *)
(* (src/scroll_hold.cpp) is a behaviour of ScrollHold.tla.                 *)
(*                                                                         *)
(* tests/window/hold_walk.cpp drives the code with a seeded random walk    *)
(* and writes ScrollHoldTraceData.tla beside this module. It defines       *)
(*   Trace: one record per step: the operation (init, press, release,      *)
(*     repeat, blur, frame), its key, and after it the code's up, down,    *)
(*     holdDirection and, for a frame, the sign of holdStep's move.        *)
(*                                                                         *)
(* Each step takes the ScrollHold action the record names, with its key;   *)
(* TraceMatches requires the spec's state to be the code's. Every          *)
(* ScrollHold invariant is checked on the trace too.                       *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, ScrollHoldTraceData

VARIABLES up, down, dir, moved, last, l

H == INSTANCE ScrollHold WITH Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ up = r.up
    /\ down = r.down
    /\ dir = r.dir
    /\ moved = r.moved

TraceMatches == Matches(l)

Step(r) ==
    CASE r.op = "press" -> H!Press(r.key)
      [] r.op = "release" -> H!Release(r.key)
      [] r.op = "repeat" -> H!Repeat(r.key)
      [] r.op = "blur" -> H!Blur
      [] r.op = "frame" -> H!Frame
      [] OTHER -> FALSE

traceVars == <<up, down, dir, moved, last, l>>

TraceInit == l = 1 /\ H!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == H!TypeOK
DirFromKeys == H!DirFromKeys
RepeatKeepsVelocity == H!RepeatKeepsVelocity
ReleaseStops == H!ReleaseStops
BlurIdles == H!BlurIdles
NoInitialDelay == H!NoInitialDelay

=============================================================================
