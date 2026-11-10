------------------------------- MODULE Glide -------------------------------
(***************************************************************************)
(* The sprint board's scrolling: nexus::GlideScroller (src/scroll_glide.h) *)
(* over the page's own scroll, in whole px and whole milliseconds.         *)
(*                                                                         *)
(* The page shows its scroll `pos`, within 0..Max, and the scroller draws  *)
(* it `over` px past an end (the rubber band), so it shows at pos + over.  *)
(* The motion (mode) is idle, a coast toward `target`, a hold (an arrow    *)
(* down: its target runs on), or a spring back to the end `target`. A      *)
(* motion begun since the last frame is `pending`. `left` is how long a    *)
(* coast or a spring may still run (its cap), `dir` the way a coast or a   *)
(* hold moves. `held` is the scroller's hold; `pressed` the arrow the      *)
(* person holds down. `applied` is the scroll the scroller set last; the   *)
(* page scrolling itself (`pageMoved`, a ghost) makes pos differ from it.  *)
(*                                                                         *)
(* The curves are left free here, within what the code's own satisfy: a   *)
(* coast or a hold moves one way, a coast never past its target, a spring  *)
(* stays past its end, and each ends within its cap. The code's curves     *)
(* are unit tested (tests/window/test_scroll_glide.cpp); GlideTrace binds  *)
(* the code's steps to this module.                                        *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    Max,            \* how far the page scrolls
    Bound,          \* the rubber band stays under this (the view's height)
    Line,           \* an arrow's line
    Bump,           \* how far past an end a key or a notch there aims
    KeyDeltas,      \* what the other keys scroll by (a page, an end), signed
    WheelDeltas,    \* a notch
    PadDeltas,      \* a trackpad's scroll
    Spots,          \* where the page's own script may put its scroll
    Dts,            \* the time between frames, ms
    CoastTicks,     \* a coast's cap, ms
    SpringTicks,    \* a spring's cap, ms
    Inputs          \* how many inputs a behaviour has (the person's, and the page's own scrolls)

VARIABLES pos, over, mode, pending, target, from, dir, left, held, hdir, applied, pressed, pageMoved, act, inputs

vars == <<pos, over, mode, pending, target, from, dir, left, held, hdir, applied, pressed, pageMoved, act, inputs>>

Range == 0 .. Max
Wide == (0 - Bound) .. (Max + Bound)
Modes == {"idle", "coast", "hold", "spring"}
Clamp(x) == IF x < 0 THEN 0 ELSE IF x > Max THEN Max ELSE x
Sign(x) == IF x > 0 THEN 1 ELSE IF x < 0 THEN -1 ELSE 0
End(s) == IF s > 0 THEN Max ELSE 0
Shown == pos + over
\* b is not behind a, going the way s
Ahead(s, a, b) == s * (b - a) >= 0

\* The page scrolled itself since the scroller set it: the scroller lets its motion and its hold go.
Stale == (mode # "idle" \/ held) /\ pos # applied
Moving == ~Stale /\ mode # "idle"
Holding == ~Stale /\ held

\* An idle motion carries nothing.
Rest == /\ mode' = "idle" /\ pending' = FALSE /\ target' = 0 /\ from' = 0 /\ dir' = 0 /\ left' = 0
Input == inputs > 0 /\ inputs' = inputs - 1

TypeOK ==
    /\ pos \in Range /\ applied \in Range
    /\ over \in (1 - Bound) .. (Bound - 1)
    /\ mode \in Modes /\ pending \in BOOLEAN
    /\ target \in Wide /\ from \in Wide /\ dir \in {-1, 0, 1} /\ left \in 0 .. CoastTicks
    /\ held \in BOOLEAN /\ hdir \in {-1, 0, 1} /\ pressed \in BOOLEAN /\ pageMoved \in BOOLEAN
    /\ inputs \in 0 .. Inputs

Init ==
    /\ pos \in Range /\ applied = pos /\ over = 0
    /\ mode = "idle" /\ pending = FALSE /\ target = 0 /\ from = 0 /\ dir = 0 /\ left = 0
    /\ held = FALSE /\ hdir = 0 /\ pressed = FALSE /\ pageMoved = FALSE
    /\ act = "init" /\ inputs = Inputs

(***************************************************************************)
(* An event through Prax's view (GlideScroller::through): from where the   *)
(* motion goes if it goes the event's way s, else from where the page      *)
(* shows; the page scrolls by d there, within its range (Cl). If it moved, *)
(* a coast there from where the page shows; if not and it is at the end it *)
(* goes toward, a coast Bump past that end. The page does not move at the  *)
(* event. m, pd, tg, dr, ov are the motion as the event finds it (after a  *)
(* stale motion is let go, or a hold released).                            *)
(***************************************************************************)
Aim(s, m, tg, dr) == IF m \notin {"idle", "spring"} /\ s # 0 /\ dr = s THEN Clamp(tg) ELSE pos
Lands(Cl(_), d, s, m, tg, dr) == Cl(Aim(s, m, tg, dr) + d)
Moves(Cl(_), d, s, m, tg, dr) == Lands(Cl, d, s, m, tg, dr) # Aim(s, m, tg, dr)

ThroughWith(Cl(_), d, s, m, pd, tg, dr, ov) ==
    LET aim == Aim(s, m, tg, dr)
        new == Lands(Cl, d, s, m, tg, dr)
        moved == new # aim
        bump == ~moved /\ s # 0 /\ aim = End(s) /\ ov = 0
        shown == pos + ov
    IN /\ pos' = pos /\ over' = ov /\ applied' = pos
       /\ IF moved \/ bump
          THEN /\ mode' = "coast"
               /\ target' = IF moved THEN new ELSE End(s) + s * Bump
               /\ pending' = (m = "idle" \/ pd)
               /\ from' = shown
               /\ dir' = IF moved /\ Sign(new - shown) # 0 THEN Sign(new - shown) ELSE s
               /\ left' = CoastTicks
          ELSE IF m # "idle" THEN /\ mode' = m /\ pending' = pd /\ target' = tg /\ from' = from /\ dir' = dr
                                  /\ left' = left
                             ELSE Rest

\* The motion an event finds: a stale one let go.
Found(Cl(_), d, s) ==
    IF Stale THEN ThroughWith(Cl, d, s, "idle", FALSE, 0, 0, 0)
             ELSE ThroughWith(Cl, d, s, mode, pending, target, dir, over)

\* A hold released (GlideScroller::release): it coasts on, from where it shows, to a target ahead of it.
ReleasedTo(t) == t \in Wide /\ Ahead(hdir, Shown, t)

\* Another key or a notch: a hold is released first, then the event.
EventWith(Cl(_), d, name) ==
    /\ Input /\ act' = name
    /\ LET s == Sign(d) IN
       IF Holding /\ mode = "hold"
       THEN \E t \in Wide : ReleasedTo(t) /\ ThroughWith(Cl, d, s, "coast", pending, t, hdir, over)
       ELSE Found(Cl, d, s)
    /\ held' = FALSE /\ hdir' = (IF Holding THEN 0 ELSE hdir)
    /\ UNCHANGED <<pressed, pageMoved>>
KeyWith(Cl(_), d) == EventWith(Cl, d, "key")
WheelWith(Cl(_), d) == EventWith(Cl, d, "wheel")

\* An arrow goes down (not a repeat): a line its way; if the page moved, a hold.
PressWith(Cl(_), s) ==
    /\ Input /\ ~pressed /\ pressed' = TRUE /\ act' = "press"
    /\ LET m == IF Stale THEN "idle" ELSE mode
           tg == IF Stale THEN 0 ELSE target
           dr == IF Stale THEN 0 ELSE dir
       IN IF Moves(Cl, s * Line, s, m, tg, dr)
          THEN /\ pos' = pos /\ over' = (IF Stale THEN 0 ELSE over) /\ applied' = pos
               /\ mode' = "hold" /\ target' = Lands(Cl, s * Line, s, m, tg, dr)
               /\ pending' = (m = "idle" \/ (~Stale /\ pending))
               /\ from' = pos + (IF Stale THEN 0 ELSE over) /\ dir' = s /\ left' = 0
               /\ held' = TRUE /\ hdir' = s
          ELSE /\ Found(Cl, s * Line, s)
               /\ held' = FALSE /\ hdir' = hdir
    /\ UNCHANGED pageMoved

\* The OS's repeat of a held arrow: the scroller ignores it, and the page never sees it.
Repeat ==
    /\ Input /\ pressed /\ act' = "repeat"
    /\ UNCHANGED <<pos, over, mode, pending, target, from, dir, left, held, hdir, applied, pressed, pageMoved>>

\* The arrow comes up, or the window loses focus (Prax sends the held key's release).
ReleaseWith(Keep) ==
    /\ pressed /\ pressed' = FALSE /\ act' = "release"
    /\ UNCHANGED <<inputs, pageMoved>>
    /\ IF Stale
       THEN Rest /\ held' = FALSE /\ over' = 0 /\ UNCHANGED <<pos, applied, hdir>>
       ELSE IF held /\ mode = "hold" /\ ~Keep
       THEN /\ mode' = "coast" /\ target' \in Wide /\ ReleasedTo(target')
            /\ from' = Shown /\ dir' = hdir /\ left' = CoastTicks /\ pending' = pending
            /\ held' = FALSE
            /\ UNCHANGED <<pos, over, applied, hdir>>
       ELSE /\ held' = (held /\ Keep)
            /\ UNCHANGED <<pos, over, mode, pending, target, from, dir, left, applied, hdir>>
Release == ReleaseWith(FALSE)

\* A trackpad's scroll (d) or a press (d = 0): the motion and the hold stop where the page shows, and the page scrolls.
InterruptWith(Cancel, d, name) ==
    /\ Input /\ act' = name
    /\ pos' = Clamp(pos + d) /\ applied' = pos'
    /\ IF Cancel \/ Stale
       THEN Rest /\ held' = FALSE /\ over' = 0
       ELSE UNCHANGED <<mode, pending, target, from, dir, left, held, over>>
    /\ UNCHANGED <<hdir, pressed>>
    /\ pageMoved' = FALSE

\* The page's own script moves its scroll.
Page(y) ==
    /\ Input /\ y # pos /\ act' = "page"
    /\ pos' = y /\ pageMoved' = TRUE
    /\ UNCHANGED <<over, mode, pending, target, from, dir, left, held, hdir, applied, pressed>>

\* The page leaves the screen: the motion ends where it was going, the hold ends.
Hide ==
    /\ Input /\ act' = "hide"
    /\ IF Moving
       THEN IF mode = "hold" THEN pos' \in Range /\ Ahead(hdir, pos, pos') ELSE pos' = Clamp(target)
       ELSE pos' = pos
    /\ applied' = pos' /\ over' = 0 /\ Rest /\ held' = FALSE
    /\ UNCHANGED <<hdir, pressed, pageMoved>>

(***************************************************************************)
(* A frame (GlideScroller::frame). A coast or a hold moves its way, a      *)
(* coast never past a target within the page; past an end the band shows  *)
(* only while it comes back in. One that carries the page past an end      *)
(* becomes a spring there. A coast comes to rest on its target, at the     *)
(* latest when its cap runs out. A spring stays past its end (SpringGo)    *)
(* and comes to rest on it, at the latest when its cap runs out. A hold    *)
(* parked at an end, or no motion, does nothing.                           *)
(***************************************************************************)
Banded(p, o) == /\ p \in Range /\ o \in (1 - Bound) .. (Bound - 1)
                /\ (o > 0 => p = Max) /\ (o < 0 => p = 0)

SpringGo(l) ==
    /\ l > 0 /\ left' = l
    /\ pos' = target
    /\ over' \in (1 - Bound) .. (Bound - 1) /\ (IF target = Max THEN over' >= 0 ELSE over' <= 0)

FrameWith(Cl(_), SpringStep(_), dt) ==
    /\ dt \in Dts /\ act' = "frame"
    /\ UNCHANGED <<pressed, inputs, hdir>>
    /\ pageMoved' = FALSE
    /\ IF Stale THEN Rest /\ held' = FALSE /\ over' = 0 /\ UNCHANGED <<pos, applied>>
       ELSE IF mode = "idle" THEN UNCHANGED <<pos, over, mode, pending, target, from, dir, left, held, applied>>
       ELSE LET l == IF pending THEN left ELSE left - dt IN
            /\ IF mode = "spring"
               THEN \/ SpringStep(l) /\ UNCHANGED <<mode, target, from, dir>> /\ pending' = FALSE
                    \/ pos' = target /\ over' = 0 /\ Rest
               ELSE \/ \* on its way
                       /\ mode = "hold" \/ l > 0
                       /\ Banded(pos', over') /\ Ahead(dir, Shown, pos' + over')
                       /\ over' # 0 => Sign(over') = -dir
                       /\ mode = "coast" => Ahead(dir, pos' + over', Clamp(target))
                       /\ left' = (IF mode = "coast" THEN l ELSE 0)
                       /\ pending' = FALSE /\ UNCHANGED <<mode, target, from, dir>>
                    \/ \* at rest on its target
                       /\ mode = "coast"
                       /\ pos' = Cl(target) /\ over' = 0 /\ Rest
                    \/ \* carried past an end: the spring
                       /\ dir # 0
                       /\ pos' = End(dir) /\ over' \in (1 - Bound) .. (Bound - 1) /\ dir * over' >= 0
                       /\ Ahead(dir, Shown, pos' + over')
                       /\ mode' = "spring" /\ target' = End(dir) /\ from' = Shown /\ dir' = 0
                       /\ left' = SpringTicks /\ pending' = FALSE
            /\ applied' = pos' /\ held' = held

NextWith(P(_), K(_), W(_), R, F(_), I(_, _)) ==
    \/ \E s \in {-1, 1} : P(s)
    \/ \E d \in KeyDeltas : K(d)
    \/ \E d \in WheelDeltas : W(d)
    \/ Repeat
    \/ R
    \/ \E dt \in Dts : F(dt)
    \/ \E d \in PadDeltas : I(d, "pad")
    \/ I(0, "click")
    \/ \E y \in Spots : Page(y)
    \/ Hide

Press(s) == PressWith(Clamp, s)
Key(d) == KeyWith(Clamp, d)
Wheel(d) == WheelWith(Clamp, d)
Frame(dt) == FrameWith(Clamp, SpringGo, dt)
Interrupt(d, name) == InterruptWith(TRUE, d, name)

Next == NextWith(Press, Key, Wheel, Release, Frame, Interrupt)

\* Frames keep coming, and a key held down comes up (or the window loses focus).
Fairness == WF_vars(\E dt \in Dts : Frame(dt)) /\ WF_vars(Release)
Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)
\* The page's scroll within its range, the band under the view's height.
InRange == pos \in Range /\ applied \in Range /\ over \in (1 - Bound) .. (Bound - 1)
\* The band only at an end, on that end's side.
BandAtEnds == ~pageMoved => /\ over > 0 => pos = Max
                            /\ over < 0 => pos = 0
\* While it moves the page, the page shows where the scroller put it, unless the page moved itself.
ShowsItself == ((mode # "idle" \/ held) /\ ~pageMoved) => pos = applied
\* A spring stays at its end, its band on the outside: it never swings back into the page.
SpringAtEnd == (mode = "spring" /\ ~pageMoved) =>
    /\ target \in {0, Max} /\ pos = target
    /\ IF target = Max THEN over >= 0 ELSE over <= 0
\* Only an arrow held down holds, and only a hold is held.
HeldOnlyPressed == held => pressed
HoldIsHeld == mode = "hold" => held

Events == {"press", "key", "wheel", "release", "repeat"}
\* An event never moves the page: what it starts moves from where the page shows.
EventsKeepPlace == [][(~Stale /\ act' \in Events) => (pos' = pos /\ over' = over)]_vars
\* A coast or a hold, and every event or frame during one, never moves the page back against its way.
NeverBack == [][(Moving /\ mode \in {"coast", "hold"} /\ act' \in Events \cup {"frame"}) =>
                 dir * ((pos' + over') - Shown) >= 0]_vars
\* A trackpad's scroll or a press of the pointer stops the motion and the hold: the page is the person's again.
PadStops == [][act' \in {"pad", "click"} => (mode' = "idle" /\ ~held' /\ over' = 0)]_vars
\* Once the input stops, the page comes to rest and nothing is held.
Settles == <>[](mode = "idle" /\ ~held)
=============================================================================
