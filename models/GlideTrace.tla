----------------------------- MODULE GlideTrace -----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real                 *)
(* nexus::GlideScroller (src/scroll_glide.h) is a behaviour of Glide.tla.  *)
(*                                                                         *)
(* tests/model/glide_walk.cpp drives the scroller over a page in whole px  *)
(* with a seeded random walk of presses, repeats, releases, other keys,    *)
(* notches, trackpad scrolls, presses of the pointer, the page's own       *)
(* scrolls, hides and frames, and writes GlideTraceData.tla beside this    *)
(* module: the page's constants (TraceMax and the rest) and Trace, one     *)
(* record per step:                                                        *)
(* the operation and its argument, and the scroller's state after it (the *)
(* page's scroll, the band rounded to px, the motion, its target and how   *)
(* many ms its cap has left, the hold, the scroll it set last).            *)
(*                                                                         *)
(* Each step pins the spec's next state to the record and takes the Glide  *)
(* action the record names. A step the spec cannot take to that state has  *)
(* no successor, which TLC reports as a deadlock. Glide's invariants and   *)
(* action properties are checked on the trace as well.                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, GlideTraceData

VARIABLES pos, over, mode, pending, target, from, dir, left, held, hdir, applied, pressed, pageMoved, act, inputs, l

G == INSTANCE Glide WITH Max <- TraceMax, Bound <- TraceBound, Line <- TraceLine, Bump <- TraceBump,
                         KeyDeltas <- TraceKeyDeltas, WheelDeltas <- TraceWheelDeltas, PadDeltas <- TracePadDeltas,
                         Spots <- TraceSpots, Dts <- TraceDts, CoastTicks <- TraceCoastTicks,
                         SpringTicks <- TraceSpringTicks, Inputs <- Len(Trace)

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

\* The next state is the code's: what the scroller holds, where the motion counts it.
Pin(s) ==
    /\ pos' = s.pos /\ over' = s.over /\ mode' = s.mode /\ held' = s.held
    /\ s.mode # "idle" => (pending' = s.pending /\ target' = s.target /\ left' = s.left)
    /\ (s.mode # "idle" \/ s.held) => applied' = s.applied

\* The Glide action a record names, with its argument.
Step(s) ==
    CASE s.op = "press"   -> s.s \in {-1, 1} /\ G!Press(s.s)
      [] s.op = "repeat"  -> G!Repeat
      [] s.op = "release" -> G!Release
      [] s.op = "key"     -> s.d \in TraceKeyDeltas /\ G!Key(s.d)
      [] s.op = "wheel"   -> s.d \in TraceWheelDeltas /\ G!Wheel(s.d)
      [] s.op = "pad"     -> s.d \in TracePadDeltas /\ G!Interrupt(s.d, "pad")
      [] s.op = "click"   -> G!Interrupt(0, "click")
      [] s.op = "page"    -> s.y \in TraceSpots /\ G!Page(s.y)
      [] s.op = "hide"    -> G!Hide
      [] s.op = "frame"   -> s.dt \in TraceDts /\ G!Frame(s.dt)
      [] OTHER            -> FALSE

traceVars == <<pos, over, mode, pending, target, from, dir, left, held, hdir, applied, pressed, pageMoved, act, inputs, l>>

TraceInit == l = 1 /\ G!Init /\ pos = Trace[1].pos

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Pin(Trace[l + 1])
    /\ Step(Trace[l + 1])

\* The end of the trace: stutter, so that a trace read to its end is no deadlock.
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == G!TypeOK
InRange == G!InRange
BandAtEnds == G!BandAtEnds
ShowsItself == G!ShowsItself
SpringAtEnd == G!SpringAtEnd
HeldOnlyPressed == G!HeldOnlyPressed
HoldIsHeld == G!HoldIsHeld
EventsKeepPlace == G!EventsKeepPlace
NeverBack == G!NeverBack
PadStops == G!PadStops
=============================================================================
