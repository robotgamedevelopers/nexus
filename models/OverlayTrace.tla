---------------------------- MODULE OverlayTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real                 *)
(* nexus::OverlayRules is a behaviour of Overlay.tla.                      *)
(*                                                                         *)
(* tests/model/overlay_walk.cpp drives the rules with a seeded random walk *)
(* over a window that carries out their answers as main.cpp's carryOut    *)
(* does, and writes OverlayTraceData.tla beside this module. It defines    *)
(* Trace: one record per step. Trace[1] is the start (op "init"); each     *)
(* later record is the input, the act the rules answered, and the state    *)
(* after it: visible, active, the rules' keyed() and whether the key check *)
(* is pending.                                                             *)
(*                                                                         *)
(* Each step takes the Overlay action the record names. The OS's choice    *)
(* (whether a show got the keyboard) is the record's. The invariant        *)
(* TraceMatches requires the spec's state and act to equal the code's; a  *)
(* step the spec cannot take is a deadlock.                                *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, OverlayTraceData

VARIABLES visible, active, keyed, armed, last, held, l

O == INSTANCE Overlay

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET s == Trace[k] IN
    /\ visible = s.visible
    /\ active = s.active
    /\ keyed = s.keyed
    /\ armed = s.armed
    /\ last = [op |-> s.op, act |-> s.act]
    \* the walk's window exists from the start, as main.cpp start's does
    /\ held

TraceMatches == Matches(l)

\* the Overlay action a trace record names; the OS's answer to a show is the record's
Step(s) ==
    /\ active' = s.active
    /\ CASE s.op = "hotkey"   -> O!Hotkey
         [] s.op = "escape"   -> O!Escape
         [] s.op = "observe"  -> O!Observe
         [] s.op = "keycheck" -> O!KeyCheck
         [] s.op = "lose"     -> O!Lose
         [] s.op = "click"    -> O!Click
         [] OTHER             -> FALSE

traceVars == <<visible, active, keyed, armed, last, held, l>>

TraceInit == l = 1 /\ O!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* Overlay's spec (without its fairness, which a finite trace cannot meet) and its properties, checked on the trace
OverlaySpec == O!SafeSpec
TypeOK == O!TypeOK
KeyedOnlyVisible == O!KeyedOnlyVisible
UnkeyedIsChecked == O!UnkeyedIsChecked
HotkeyShows == O!HotkeyShows
HotkeyHides == O!HotkeyHides
EscapeHides == O!EscapeHides
NoHideWhileActive == O!NoHideWhileActive
NoHideBeforeKey == O!NoHideBeforeKey
LossHides == O!LossHides
RefusedShowHides == O!RefusedShowHides

=============================================================================
