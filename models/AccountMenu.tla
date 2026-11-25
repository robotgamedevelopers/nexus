----------------------------- MODULE AccountMenu -----------------------------
(***************************************************************************)
(* The account menu's one gesture (src/account_menu.h AccountMenuGesture,  *)
(* docs/accounts.md "Switching accounts"). A press on the active AI's tab  *)
(* opens the menu under the tab's highlight; the finger slides down onto   *)
(* a row and lifts to choose it. Lifting anywhere that is not a row closes *)
(* the menu and changes nothing: no menu is left open for a tap. A move    *)
(* mostly sideways past the threshold closes the menu and the press        *)
(* becomes the tab's move; the first move past the threshold decides, and  *)
(* once it went down onto the menu the press stays the menu's. A press     *)
(* anywhere else never opens it.                                           *)
(*                                                                         *)
(* The pointer is abstracted to where it is (the active tab, another tab,  *)
(* a row, or anywhere else) and how far a move is from the press: still   *)
(* (within the threshold), down or sideways (past it, mostly that way).    *)
(* phase is the gesture's; hovered the row lit; chosen the row a lift      *)
(* chose (None after any other event); moving that the press became the    *)
(* tab's move; decided that a move went down past the threshold; last the  *)
(* event, the phase it found and whether the press was decided then.       *)
(*                                                                         *)
(* AccountMenuTrace.tla checks a random walk of the code against it        *)
(* (tests/window/account_menu_walk.cpp); witnesses/WAccountMenu*.tla are   *)
(* the reversed witnesses.                                                 *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Rows, None

Places == {"active", "other", "off"} \cup Rows
Ways == {"still", "down", "sideways"}
Phases == {"closed", "open", "tabmove"}

VARIABLES phase, hovered, chosen, moving, decided, last

vars == <<phase, hovered, chosen, moving, decided, last>>

Init ==
    /\ phase = "closed"
    /\ hovered = None
    /\ chosen = None
    /\ moving = FALSE
    /\ decided = FALSE
    /\ last = [op |-> "init", at |-> "off", from |-> "closed", decided |-> FALSE]

Did(op, at) == last' = [op |-> op, at |-> at, from |-> phase, decided |-> decided]

\* A press. Only one on the active AI's tab opens the menu; a press while it is open (a second finger) closes it.
PressAt(at) ==
    /\ phase' = IF phase # "open" /\ at = "active" THEN "open" ELSE "closed"
    /\ hovered' = None
    /\ chosen' = None
    /\ moving' = FALSE
    /\ decided' = FALSE
    /\ Did("press", at)

\* The finger moves. Open and undecided: sideways past the threshold the menu closes and the tab moves. Otherwise, open,
\* the row under it is lit, and a move down past the threshold decides the press for the menu.
MoveTo(at, way) ==
    /\ chosen' = None
    /\ IF phase = "open"
         THEN IF way = "sideways" /\ ~decided
                THEN /\ phase' = "tabmove" /\ hovered' = None /\ moving' = TRUE /\ UNCHANGED decided
                ELSE /\ phase' = "open" /\ hovered' = IF at \in Rows THEN at ELSE None /\ UNCHANGED moving
                     /\ decided' = (decided \/ way = "down")
         ELSE UNCHANGED <<phase, hovered, moving, decided>>
    /\ Did(IF way = "sideways" THEN "side" ELSE "move", at)

\* The finger lifts. Open: the row under it is chosen, if any; the menu closes either way.
LiftAt(at) ==
    /\ phase' = "closed"
    /\ hovered' = None
    /\ chosen' = IF phase = "open" /\ at \in Rows THEN at ELSE None
    /\ moving' = FALSE
    /\ UNCHANGED decided
    /\ Did("lift", at)

\* The system takes the pointer.
Cancel ==
    /\ phase' = "closed" /\ hovered' = None /\ chosen' = None /\ moving' = FALSE
    /\ UNCHANGED decided
    /\ Did("cancel", "off")

Press == \E at \in Places : PressAt(at)
Move == \E at \in Places, way \in Ways : MoveTo(at, way)
Lift == \E at \in Places : LiftAt(at)

NextWith(P, M, L) == P \/ M \/ L \/ Cancel
Next == NextWith(Press, Move, Lift)
Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ phase \in Phases
    /\ hovered \in Rows \cup {None}
    /\ chosen \in Rows \cup {None}
    /\ moving \in BOOLEAN
    /\ decided \in BOOLEAN
    /\ last.op \in {"init", "press", "move", "side", "lift", "cancel"} /\ last.at \in Places /\ last.from \in Phases
    /\ last.decided \in BOOLEAN

\* Only a press on the active AI's tab, with nothing open, opens the menu.
OpensOnlyOnTheActiveTab == phase = "open" /\ last.op = "press" => last.at = "active" /\ last.from # "open"

\* A choice is made only by a lift on a row of an open menu.
ChoiceOnlyByLiftOnARow == chosen # None => last.op = "lift" /\ last.at = chosen /\ last.from = "open"

\* A lift always closes the menu: no menu is left open for a tap.
LiftCloses == last.op = "lift" => phase = "closed"

\* A press that became the tab's move never chooses, and never shows the menu again before the lift.
TabMoveNeverChooses == moving => phase # "open" /\ chosen = None

\* A move sideways from an open menu not yet decided is the tab's move: the menu is gone.
SidewaysIsTheTabsMove == last.op = "side" /\ last.from = "open" /\ ~last.decided => phase = "tabmove"

\* Once the finger went down onto the menu, a slide sideways (across a wide row) keeps the menu: the tab never moves.
DecidedStaysTheMenu == last.op = "side" /\ last.from = "open" /\ last.decided => phase = "open"

\* Only an open menu lights a row.
LitOnlyWhileOpen == hovered # None => phase = "open"
=============================================================================
