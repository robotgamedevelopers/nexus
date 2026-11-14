------------------------------ MODULE Takeover ------------------------------
(***************************************************************************)
(* A newer build of nexus-server taking over from the running one:         *)
(* src/server/takeover.h (nextStep, the installer's side; Drain, the       *)
(* running server's), as `nexus-server setup` and `rebuild` drive them.    *)
(*                                                                         *)
(* The installer places a build (Place), then looks at the server and      *)
(* takes the step nextStep gives (Ask): a server from before takeover      *)
(* (OldBuild, which does not answer `build`) is stopped; any other server  *)
(* running a build other than the placed one is asked to drain. A         *)
(* draining server stops at the first moment no turn is open, or, once    *)
(* its deadline has passed (Late), with a turn open, cutting it. When no   *)
(* server runs, the service starts the placed build (Start). Friends'      *)
(* turns open and end on whatever server runs (TurnOpen, TurnEnd).         *)
(*                                                                         *)
(* State:                                                                  *)
(*   placed   the build at ~/.nexus/bin                                    *)
(*   server   the running server's build, None when none runs              *)
(*   draining whether it was asked to drain                                *)
(*   late     whether its drain's deadline has passed                      *)
(*   turn     whether a friend's turn is open on it                        *)
(*   places   how many builds were placed (bounds the model)               *)
(*                                                                         *)
(* Properties:                                                             *)
(*   DrainCutsOnlyLate  a draining server never stops with a turn open     *)
(*                      before its deadline                                *)
(*   StopOnlyOld        a server is stopped outright (without draining)    *)
(*                      only when it is from before takeover               *)
(*   StartsPlaced       the service starts only the placed build           *)
(*   PlacedRuns         once the builds stop changing, the placed build    *)
(*                      comes to run, and stays                            *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    Builds,     \* the builds a rebuild places, each answering `build`
    OldBuild,   \* a build from before takeover: it answers neither `build` nor `takeover`
    None,       \* no server runs
    MaxPlaces   \* how many builds the installer places, at most

ASSUME OldBuild \notin Builds /\ None \notin Builds /\ None # OldBuild

VARIABLES placed, server, draining, late, turn, places

vars == <<placed, server, draining, late, turn, places>>

AllBuilds == Builds \cup {OldBuild}

TypeOK ==
    /\ placed \in AllBuilds
    /\ server \in AllBuilds \cup {None}
    /\ draining \in BOOLEAN
    /\ late \in BOOLEAN
    /\ turn \in BOOLEAN
    /\ places \in 0..MaxPlaces
    /\ (server = None) => (~draining /\ ~late /\ ~turn)
    /\ late => draining

\* An old install: the old build placed, and running or not.
Init ==
    /\ placed = OldBuild
    /\ server \in {OldBuild, None}
    /\ draining = FALSE
    /\ late = FALSE
    /\ turn = FALSE
    /\ places = 0

\* setup or rebuild places a build other than the one placed.
Place(b) ==
    /\ places < MaxPlaces
    /\ b # placed
    /\ placed' = b
    /\ places' = places + 1
    /\ UNCHANGED <<server, draining, late, turn>>

\* The server stops: its turn, if open, is cut (the next server cuts it with a notice: models/Supervisor.tla).
Gone ==
    /\ server' = None
    /\ draining' = FALSE
    /\ late' = FALSE
    /\ turn' = FALSE

\* nextStep's AskStop and AskDrain: the installer acts on a server running a build other than the placed one.
Ask ==
    /\ server # None
    /\ server # placed
    /\ IF server = OldBuild
       THEN /\ Gone
       ELSE /\ ~draining
            /\ draining' = TRUE
            /\ UNCHANGED <<server, late, turn>>
    /\ UNCHANGED <<placed, places>>

\* Drain::late: the deadline passes.
Late ==
    /\ server # None
    /\ draining
    /\ ~late
    /\ late' = TRUE
    /\ UNCHANGED <<placed, server, draining, turn, places>>

\* Drain::stopNow: no turn open, or past the deadline.
DrainStop ==
    /\ server # None
    /\ draining
    /\ ~turn \/ late
    /\ Gone
    /\ UNCHANGED <<placed, places>>

\* nextStep's Start: the service (or setup) starts the placed program.
Start ==
    /\ server = None
    /\ server' = placed
    /\ UNCHANGED <<placed, draining, late, turn, places>>

TurnOpen ==
    /\ server # None
    /\ ~turn
    /\ turn' = TRUE
    /\ UNCHANGED <<placed, server, draining, late, places>>

TurnEnd ==
    /\ turn
    /\ turn' = FALSE
    /\ UNCHANGED <<placed, server, draining, late, places>>

Next ==
    \/ \E b \in AllBuilds : Place(b)
    \/ Ask
    \/ Late
    \/ DrainStop
    \/ Start
    \/ TurnOpen
    \/ TurnEnd

\* Turns may keep opening forever: no fairness on TurnOpen or TurnEnd. The installer, the deadline, the drain and the
\* service are each sure to act.
Fairness == WF_vars(Ask) /\ WF_vars(Late) /\ WF_vars(DrainStop) /\ WF_vars(Start)

Spec == Init /\ [][Next]_vars /\ Fairness

\* A draining server that stops with a turn open does so only past its deadline.
DrainCutsOnlyLate ==
    [][(draining /\ server' = None /\ turn) => late]_vars

\* A server that was not asked to drain is stopped only when it is from before takeover.
StopOnlyOld ==
    [][(server # None /\ ~draining /\ server' = None) => server = OldBuild]_vars

\* What starts is the placed build.
StartsPlaced ==
    [][(server = None /\ server' # None) => server' = placed]_vars

\* Once nothing more is placed, the placed build comes to run and stays.
PlacedRuns ==
    <>[](server = placed)

=============================================================================
