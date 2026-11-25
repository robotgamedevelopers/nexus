---------------------------- MODULE FingerScroll ----------------------------
(***************************************************************************)
(* The iPhone's list under the finger: nexus::FingerScroll                 *)
(* (src/finger_scroll.h), its offsets in whole rows.                       *)
(*                                                                         *)
(* The list scrolls over the range 0..max (max: how many rows the content  *)
(* is longer than the view) and shows at `off`, past an end while a finger *)
(* drags it there (rubber-banded, under Band rows: the view's height) or   *)
(* while it springs back. The motion (phase) is one of                     *)
(*   rest    still, inside the range;                                      *)
(*   track   a finger down that has not moved past the slop (maybe a tap); *)
(*   drag    a finger moving the list;                                     *)
(*   fling   let go moving, inside: it goes on its way `dir` and slows;    *)
(*   spring  past the end on its `side` (-1 the top, 1 the bottom), back   *)
(*           to that end, `send`; while `stage` is "out" a speed away from *)
(*           the end still carries it on out, then it comes "back".        *)
(* `caught` is whether the last press caught a moving list; `tap` what the *)
(* last up returned (the press was a tap: the row under it takes it).      *)
(*                                                                         *)
(* The curves are abstracted to the ways they move, which the code's keep  *)
(* (the curves themselves are unit tested, tests/voice/                    *)
(* test_finger_scroll.cpp): a fling moves one way and comes to rest        *)
(* inside, or reaches the end it goes to and springs there; a spring stays *)
(* past its end, first on out while its speed is away from it and then     *)
(* back, and comes to rest on it. The range changes under any of these     *)
(* (setRange, each frame: the rows come and go). FingerScrollTrace binds   *)
(* the code's steps to this module.                                        *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    Maxes,      \* the ranges the list may have, in rows
    Band,       \* a drag past an end shows under this many rows past it (the rubber band, under the view's height)
    Bound,      \* how far past an end the list ever shows (a spring carried on out), in rows
    Inputs      \* how many of the finger's events and range changes a behaviour has

VARIABLES phase, off, max, caught, dir, send, side, stage, tap, act, inputs

vars == <<phase, off, max, caught, dir, send, side, stage, tap, act, inputs>>

MaxCap == CHOOSE m \in Maxes : \A n \in Maxes : n <= m
Wide == (0 - Bound) .. (MaxCap + Bound)
Phases == {"rest", "track", "drag", "fling", "spring"}
Inside(o, m) == 0 <= o /\ o <= m
Clamp(o, m) == IF o < 0 THEN 0 ELSE IF o > m THEN m ELSE o
\* The end on a side: the top (-1) or the bottom (1) of the range 0..m.
EndOf(s, m) == IF s < 0 THEN 0 ELSE m
\* b is not behind a, going the way d.
Ahead(d, a, b) == d * (b - a) >= 0
\* o is on the side s of the end e, or on it: outside the range there.
Outside(s, e, o) == s * (o - e) >= 0
\* Where a finger's drag shows the list: inside, or rubber-banded under Band rows past an end.
Banded(o, m) == (0 - Band) <= o /\ o <= m + Band

\* No motion: a fling's way and a spring's end, side and stage are unset.
NoMotion == dir' = 0 /\ send' = 0 /\ side' = 0 /\ stage' = "none"
\* A spring back to the end on side s of the range 0..m.
SpringTo(s, m, st) == phase' = "spring" /\ side' = s /\ send' = EndOf(s, m) /\ stage' = st /\ dir' = 0
Input(name) == inputs > 0 /\ inputs' = inputs - 1 /\ act' = name

TypeOK ==
    /\ phase \in Phases /\ off \in Wide /\ max \in Maxes /\ caught \in BOOLEAN
    /\ dir \in {-1, 0, 1} /\ send \in 0 .. MaxCap /\ side \in {-1, 0, 1} /\ stage \in {"none", "out", "back"}
    /\ tap \in BOOLEAN /\ inputs \in 0 .. Inputs

Init ==
    /\ phase = "rest" /\ max \in Maxes /\ off \in 0 .. max /\ caught = FALSE
    /\ dir = 0 /\ send = 0 /\ side = 0 /\ stage = "none" /\ tap = FALSE
    /\ act = "init" /\ inputs = Inputs

(***************************************************************************)
(* FingerScroll::release: the finger lifted from a drag (or a press past   *)
(* an end). Past an end, a spring back to it, which a speed away from it   *)
(* carries on out first (Fl: the finger's speed counts; a cancel's, and a  *)
(* press's that never moved, is none). Inside, at rest where it shows, or  *)
(* (Fl) a fling either way.                                                *)
(***************************************************************************)
Release(Fl) ==
    /\ off' = off
    /\ IF ~Inside(off, max)
       THEN \E st \in (IF Fl THEN {"out", "back"} ELSE {"back"}) : SpringTo(IF off < 0 THEN -1 ELSE 1, max, st)
       ELSE \/ phase' = "rest" /\ NoMotion
            \/ /\ Fl /\ phase' = "fling" /\ dir' \in {-1, 1}
               /\ send' = 0 /\ side' = 0 /\ stage' = "none"

\* A finger down: it catches a moving list where it shows (the code shows the motion at the press's time first: the
\* walk records that frame as a tick of its own).
Down ==
    /\ Input("down") /\ tap' = FALSE
    /\ phase' = "track" /\ caught' = (phase \in {"fling", "spring"})
    /\ UNCHANGED <<off, max>> /\ NoMotion

\* The finger moves: within the slop a track stays a maybe-tap; past it, or dragging, the list shows where the finger
\* has it, rubber-banded past an end. Without a finger down, nothing.
Move ==
    /\ Input("move") /\ tap' = FALSE
    /\ UNCHANGED <<max, caught, dir, send, side, stage>>
    /\ IF phase \in {"track", "drag"}
       THEN \/ phase = "track" /\ UNCHANGED <<phase, off>>
            \/ phase' = "drag" /\ off' \in Wide /\ Banded(off', max)
       ELSE UNCHANGED <<phase, off>>

\* The finger lifts. A press that never moved past the slop is a tap, unless it caught a moving list; past an end it
\* springs back (with no speed). A drag is never a tap: let go, it rests, flings or springs.
UpWith(TrackTap(_), DragTap(_)) ==
    /\ Input("up") /\ UNCHANGED <<max, caught>>
    /\ CASE phase = "track" -> /\ tap' = TrackTap(caught)
                               /\ IF Inside(off, max) THEN phase' = "rest" /\ off' = off /\ NoMotion
                                                      ELSE Release(FALSE)
         [] phase = "drag"  -> tap' = DragTap(caught) /\ Release(TRUE)
         [] OTHER           -> tap' = FALSE /\ UNCHANGED <<phase, off, dir, send, side, stage>>
Up == UpWith(LAMBDA c : ~c, LAMBDA c : FALSE)

\* The press is cancelled (the OS took it): as an up with no speed, and no tap.
Cancel ==
    /\ Input("cancel") /\ tap' = FALSE /\ UNCHANGED <<max, caught>>
    /\ IF phase \in {"track", "drag"} THEN Release(FALSE) ELSE UNCHANGED <<phase, off, dir, send, side, stage>>

(***************************************************************************)
(* The range changes (setRange, each frame, from the last layout). At rest *)
(* the offset is pulled into the new range at once. A fling now past the   *)
(* bottom springs back to it from where it shows, carried on out if it was *)
(* going that way. A spring back to the bottom goes to the new bottom from *)
(* where it shows, with the speed it has (so its stage); the bottom now at *)
(* or past where it shows, the list is inside, and rests there. Otherwise  *)
(* the motion goes on: a drag shows against the new range at its next      *)
(* move.                                                                   *)
(***************************************************************************)
RangeWith(Pull, Fling, Retarget, m) ==
    /\ Input("range") /\ max' = m /\ tap' = FALSE /\ UNCHANGED caught
    /\ CASE phase = "rest" /\ Pull -> off' = Clamp(off, m) /\ UNCHANGED <<phase, dir, send, side, stage>>
         [] phase = "fling" /\ off > m /\ Fling -> off' = off /\ SpringTo(1, m, IF dir = 1 THEN "out" ELSE "back")
         [] phase = "spring" /\ side = 1 /\ send # m /\ Retarget ->
               /\ off' = off
               /\ IF off <= m THEN phase' = "rest" /\ NoMotion ELSE SpringTo(1, m, stage)
         [] OTHER -> UNCHANGED <<phase, off, dir, send, side, stage>>
SetRange(m) == RangeWith(TRUE, TRUE, TRUE, m)

(***************************************************************************)
(* A frame (FingerScroll::frame).                                          *)
(*   A fling (always inside: FlingInside) goes on its way within the       *)
(*   range, or slows to rest inside (not behind where it was), or reaches  *)
(*   the end it goes to and springs there, its speed carrying it on out.   *)
(*   A spring stays past its end: on out (not back) while its stage is     *)
(*   "out", then back (not out again); or comes to rest on its end.        *)
(*   Nothing else moves at a frame.                                        *)
(***************************************************************************)
FlingTick ==
    \/ off' \in 0 .. max /\ Ahead(dir, off, off') /\ UNCHANGED <<phase, dir, send, side, stage>>
    \/ phase' = "rest" /\ off' \in 0 .. max /\ Ahead(dir, off, off') /\ NoMotion
    \/ /\ off' \in Wide /\ Outside(dir, EndOf(dir, max), off')
       /\ \E st \in {"out", "back"} : SpringTo(dir, max, st)

SpringTick ==
    \/ phase' = "rest" /\ off' = send /\ NoMotion
    \/ /\ off' \in Wide /\ Outside(side, send, off') /\ UNCHANGED <<phase, dir, send, side>>
       /\ \/ stage = "out" /\ stage' = "out" /\ Ahead(side, off, off')
          \/ stage' = "back" /\ (stage = "back" => Ahead(0 - side, off, off'))

TickWith(F, S) ==
    /\ act' = "tick" /\ tap' = FALSE /\ UNCHANGED <<max, caught, inputs>>
    /\ CASE phase = "fling"  -> F
         [] phase = "spring" -> S
         [] OTHER            -> UNCHANGED <<phase, off, dir, send, side, stage>>
Tick == TickWith(FlingTick, SpringTick)

NextWith(D, U, R(_), T) ==
    \/ D \/ Move \/ U \/ Cancel
    \/ \E m \in Maxes : R(m)
    \/ T

Next == NextWith(Down, Up, SetRange, Tick)

\* Frames keep coming.
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)
\* At rest the list is inside its range.
AtRestInside == phase = "rest" => Inside(off, max)
\* A fling is inside the range: one the range shrank under springs back at once, from where it shows.
FlingInside == phase = "fling" => Inside(off, max)
\* A spring stays past its end (or on it): it never swings back inside the list.
SpringNeverCrossesInside == phase = "spring" => Outside(side, send, off)
\* A spring goes to an end of the range as it is now (the top, or the bottom of the range the rows now make).
SpringToItsEnd == phase = "spring" => (side \in {-1, 1} /\ send = EndOf(side, max))
\* An up after a drag never taps.
DragIsNoTap == [][(phase = "drag" /\ act' = "up") => ~tap']_vars
\* A press that caught a moving list is no tap: it only stopped it.
CaughtIsNoTap == [][(caught /\ act' = "up") => ~tap']_vars
\* A shorter range at rest pulls the offset in at once.
ShrinkPullsIn == [][(phase = "rest" /\ act' = "range") => off' = Clamp(off, max')]_vars
\* With no finger on it, a fling or a spring comes to rest (or a press catches it).
Settles == (phase \in {"fling", "spring"}) ~> (phase \in {"rest", "track"})
=============================================================================
