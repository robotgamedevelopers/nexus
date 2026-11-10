------------------------------ MODULE Overlay ------------------------------
(***************************************************************************)
(* When the overlay shows and hides: nexus::OverlayRules (src/overlay.h),  *)
(* with the window that carries out its answers and the OS around it.      *)
(*                                                                         *)
(*   visible  the window is shown (prAppWindowIsVisible)                   *)
(*   active   it has the keyboard (prAppWindowIsActive)                    *)
(*   keyed    the rules' note that it has had the keyboard since it was    *)
(*            last shown (OverlayRules::keyed)                             *)
(*   armed    the key check after a show is pending (a Prax timer)         *)
(*   last     history: the input just handled and the act answered         *)
(*                                                                         *)
(* Inputs: the hotkey, Escape, a frame (observe: a change of visible or    *)
(* active asks for one), the key check firing. The OS: a show gets the     *)
(* keyboard or not; the overlay loses it (Command-Tab, a click on another  *)
(* app); a click on the overlay gives it back.                             *)
(***************************************************************************)
EXTENDS TLC

VARIABLES visible, active, keyed, armed, last

vars == <<visible, active, keyed, armed, last>>

Acts == {"None", "Show", "Hide"}
Ops == {"init", "hotkey", "escape", "observe", "keycheck", "lose", "click"}

Did(op, a) == last' = [op |-> op, act |-> a]

TypeOK ==
    /\ visible \in BOOLEAN
    /\ active \in BOOLEAN
    /\ keyed \in BOOLEAN
    /\ armed \in BOOLEAN
    /\ last \in [op : Ops, act : Acts]

Init ==
    /\ visible = FALSE
    /\ active = FALSE
    /\ keyed = FALSE
    /\ armed = FALSE
    /\ last = [op |-> "init", act |-> "None"]

\* The window carrying out an act (main.cpp carryOut). Show covers the screen, shows the window and focuses it, which
\* the OS may or may not answer with the keyboard (`granted`); Hide hides the window, which then has no keyboard.
Carry(a, granted) ==
    CASE a = "Show" -> visible' = TRUE /\ active' = granted
      [] a = "Hide" -> visible' = FALSE /\ active' = FALSE
      [] OTHER      -> UNCHANGED <<visible, active>>

\* The key check's timer: Show arms it (prAppTimerSet replaces a pending one), Hide cancels it.
Arm(a) ==
    armed' = CASE a = "Show" -> TRUE
               [] a = "Hide" -> FALSE
               [] OTHER      -> armed

\* The rules (src/overlay.cpp). Each act but None starts the keyboard history again.
Hotkey ==
    LET a == IF visible /\ active THEN "Hide" ELSE "Show" IN
    /\ \E granted \in BOOLEAN : Carry(a, granted)
    /\ Arm(a)
    /\ keyed' = FALSE
    /\ Did("hotkey", a)

Escape ==
    LET a == IF visible THEN "Hide" ELSE "None" IN
    /\ Carry(a, FALSE)
    /\ Arm(a)
    /\ keyed' = IF a = "None" THEN keyed ELSE FALSE
    /\ Did("escape", a)

Observe ==
    LET a == IF visible /\ ~active /\ keyed THEN "Hide" ELSE "None" IN
    /\ Carry(a, FALSE)
    /\ Arm(a)
    /\ keyed' = (visible /\ active)
    /\ Did("observe", a)

\* The check fires once per show.
KeyCheck ==
    LET a == IF visible /\ ~active THEN "Hide" ELSE "None" IN
    /\ armed
    /\ Carry(a, FALSE)
    /\ armed' = FALSE
    /\ keyed' = IF visible THEN active ELSE keyed
    /\ Did("keycheck", a)

\* The OS. The rules see these at the next frame (Observe).
Lose ==
    /\ active
    /\ active' = FALSE
    /\ UNCHANGED <<visible, keyed, armed>>
    /\ Did("lose", "None")

Click ==
    /\ visible /\ ~active
    /\ active' = TRUE
    /\ UNCHANGED <<visible, keyed, armed>>
    /\ Did("click", "None")

\* A witness passes its broken action in place of the real one.
NextWith(H, E, O, K) == H \/ E \/ O \/ K \/ Lose \/ Click

Next == NextWith(Hotkey, Escape, Observe, KeyCheck)

\* Frames and the key check are taken up: each that a change of state asks for comes.
Spec == Init /\ [][Next]_vars /\ WF_vars(Observe) /\ WF_vars(KeyCheck)

\* Without the fairness: what a finite code trace is checked against.
SafeSpec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Properties

\* Only a shown overlay has had the keyboard.
KeyedOnlyVisible == keyed => visible

\* A shown overlay that has not had the keyboard always has a key check coming.
UnkeyedIsChecked == visible /\ ~keyed => armed

\* The hotkey shows the overlay (covers, shows, focuses) from every state but up with the keyboard: after Command-Tab
\* the next press shows it, not merely re-activates it.
HotkeyShows == [][last'.op = "hotkey" /\ ~(visible /\ active) => visible' /\ last'.act = "Show"]_vars

\* The hotkey hides an overlay that is up with the keyboard.
HotkeyHides == [][last'.op = "hotkey" /\ visible /\ active => ~visible']_vars

\* Escape hides the overlay.
EscapeHides == [][last'.op = "escape" => ~visible']_vars

\* A frame or the key check never hides an overlay that has the keyboard: what does not take the keyboard from it (an
\* input method's window, the emoji picker) cannot hide it.
NoHideWhileActive == [][last'.op \in {"observe", "keycheck"} /\ active => visible' = visible]_vars

\* A frame never hides an overlay that has not had the keyboard since it was shown: one just shown is not hidden
\* before the keyboard arrives.
NoHideBeforeKey == [][last'.op = "observe" /\ ~keyed => visible' = visible]_vars

\* Once it has had the keyboard, the frame that sees it lost hides the overlay (Command-Tab, a click on another app).
LossHides == [][last'.op = "observe" /\ visible /\ keyed /\ ~active => ~visible']_vars

\* The key check hides an overlay shown without the keyboard.
RefusedShowHides == [][last'.op = "keycheck" /\ visible /\ ~active => ~visible']_vars

\* The overlay never stands over other apps without the keyboard for good.
NeverStranded == (visible /\ ~active) ~> (~visible \/ active)

=============================================================================
