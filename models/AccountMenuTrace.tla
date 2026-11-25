--------------------------- MODULE AccountMenuTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real                 *)
(* nexus::AccountMenuGesture (src/account_menu.h) is a behaviour of        *)
(* AccountMenu.tla.                                                        *)
(*                                                                         *)
(* tests/window/account_menu_walk.cpp drives the gesture over a fixed      *)
(* layout (the active tab, another tab, two rows under the first, and the  *)
(* rest of the window) with a seeded random walk of presses, moves, lifts  *)
(* and cancels, and writes AccountMenuTraceData.tla beside this module:    *)
(* Trace, one record per step, the event (where it is, and for a move      *)
(* whether it went sideways past the threshold from the press) and the     *)
(* gesture after it (its phase, the row lit, the row chosen).              *)
(*                                                                         *)
(* Each step pins the spec's next state to the record and takes the        *)
(* action the record names. A step the spec cannot take to that state has  *)
(* no successor, which TLC reports as a deadlock.                          *)
(***************************************************************************)
EXTENDS Naturals, Sequences, AccountMenuTraceData

VARIABLES phase, hovered, chosen, moving, last, l

M == INSTANCE AccountMenu WITH Rows <- {"r1", "r2"}, None <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

traceVars == <<phase, hovered, chosen, moving, last, l>>

Pin(s) == phase' = s.phase /\ hovered' = s.hovered /\ chosen' = s.chosen

Step(s) ==
    CASE s.op = "press"  -> M!PressAt(s.at)
      [] s.op = "move"   -> M!MoveTo(s.at, s.way)
      [] s.op = "lift"   -> M!LiftAt(s.at)
      [] s.op = "cancel" -> M!Cancel
      [] OTHER           -> FALSE

TraceInit == l = 1 /\ M!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Pin(Trace[l + 1])
    /\ Step(Trace[l + 1])

\* The end of the trace: stutter, so that a trace read to its end is no deadlock.
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == M!TypeOK
OpensOnlyOnTheActiveTab == M!OpensOnlyOnTheActiveTab
ChoiceOnlyByLiftOnARow == M!ChoiceOnlyByLiftOnARow
LiftCloses == M!LiftCloses
TabMoveNeverChooses == M!TabMoveNeverChooses
SidewaysIsTheTabsMove == M!SidewaysIsTheTabsMove
LitOnlyWhileOpen == M!LitOnlyWhileOpen
=============================================================================
