----------------------------- MODULE ScrollHold -----------------------------
(***************************************************************************)
(* The sprint board's held Up and Down keys (src/scroll_hold.h): which of  *)
(* the two keys is down, the direction the page scrolls in, and what each  *)
(* frame moves. The key's down state drives the scroll; the system's key   *)
(* repeats change nothing.                                                  *)
(*                                                                         *)
(*   up, down   the key is down (Hold::up, Hold::down)                     *)
(*   dir        the direction: 1 down, -1 up, 0 still (holdDirection)      *)
(*   moved      the sign of the last frame's move, 0 after any other step  *)
(*              (the sign of holdStep)                                      *)
(*   last       history: the step just taken and its key                   *)
(*                                                                         *)
(* The states the design names are idle (dir = 0), holding up (dir = -1)   *)
(* and holding down (dir = 1); both keys down is idle with both held.      *)
(* Actions: Press(k), Release(k), Repeat(k), Blur, Frame. Time and the     *)
(* speed's ease are not modelled: a frame moves by the direction's sign.   *)
(* The page's place and its clamp to the scroll range are the board's      *)
(* (board.cpp stepHeld) and are not modelled.                              *)
(*                                                                         *)
(* Bug names a broken variant for a reversed witness (witnesses/*.cfg);    *)
(* "none" is the design.                                                   *)
(***************************************************************************)
EXTENDS Integers

CONSTANT Bug

ASSUME Bug \in {"none", "repeat-driven", "repeat-changes", "release-keeps", "blur-keeps", "both-latest"}

VARIABLES up, down, dir, moved, last

vars == <<up, down, dir, moved, last>>

Keys == {"up", "down"}
Acts == {"init", "press", "release", "repeat", "blur", "frame"}

KeyDir(k) == IF k = "down" THEN 1 ELSE -1

\* The direction the keys give: one key alone scrolls its way, both or neither stand still.
DirOf(u, d) == IF u = d THEN 0 ELSE IF d THEN 1 ELSE -1

Init ==
    /\ up = FALSE /\ down = FALSE /\ dir = 0 /\ moved = 0
    /\ last = [act |-> "init", key |-> "up"]

SetKey(k, isDown) ==
    /\ up' = IF k = "up" THEN isDown ELSE up
    /\ down' = IF k = "down" THEN isDown ELSE down

Press(k) ==
    /\ SetKey(k, TRUE)
    /\ dir' = CASE Bug = "repeat-driven" -> dir
                [] Bug = "both-latest" -> KeyDir(k)
                [] OTHER -> DirOf(up', down')
    /\ moved' = 0
    /\ last' = [act |-> "press", key |-> k]

Release(k) ==
    /\ SetKey(k, FALSE)
    /\ dir' = IF Bug = "release-keeps" THEN dir ELSE DirOf(up', down')
    /\ moved' = 0
    /\ last' = [act |-> "release", key |-> k]

\* A repeat of either key, held or not, at any time between frames.
Repeat(k) ==
    /\ UNCHANGED <<up, down>>
    /\ dir' = CASE Bug = "repeat-driven" /\ (IF k = "up" THEN up ELSE down) -> DirOf(up, down)
                [] Bug = "repeat-changes" -> KeyDir(k)
                [] OTHER -> dir
    /\ moved' = 0
    /\ last' = [act |-> "repeat", key |-> k]

\* The window lost the keyboard, or the page left the screen.
Blur ==
    /\ IF Bug = "blur-keeps"
           THEN UNCHANGED <<up, down, dir>>
           ELSE up' = FALSE /\ down' = FALSE /\ dir' = 0
    /\ moved' = 0
    /\ last' = [act |-> "blur", key |-> "up"]

Frame ==
    /\ UNCHANGED <<up, down, dir>>
    /\ moved' = dir
    /\ last' = [act |-> "frame", key |-> "up"]

Next ==
    \/ \E k \in Keys : Press(k) \/ Release(k) \/ Repeat(k)
    \/ Blur
    \/ Frame

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
(* Properties, all invariants over the state and `last`.                   *)

TypeOK ==
    /\ up \in BOOLEAN /\ down \in BOOLEAN
    /\ dir \in {-1, 0, 1} /\ moved \in {-1, 0, 1}
    /\ last.act \in Acts /\ last.key \in Keys

\* The direction is the keys': one key alone scrolls its way; both or neither stand still.
DirFromKeys == dir = DirOf(up, down)

\* A repeat never changes the velocity: no key and no direction changes.
RepeatKeepsVelocity == last.act = "repeat" => dir = DirOf(up, down)

\* A release never leaves the page moving the released key's way, and with no key left down it is idle.
ReleaseStops ==
    last.act = "release" => /\ dir # KeyDir(last.key)
                            /\ (~up /\ ~down) => dir = 0

\* A blur lets go of both keys: idle.
BlurIdles == last.act = "blur" => (~up /\ ~down /\ dir = 0)

\* No initial delay: whenever exactly one key is down, a frame moves the page its way, the first frame after the
\* press included.
NoInitialDelay == (last.act = "frame" /\ up # down) => moved = DirOf(up, down)

=============================================================================
