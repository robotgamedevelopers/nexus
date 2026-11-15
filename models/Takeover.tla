------------------------------ MODULE Takeover ------------------------------
(***************************************************************************)
(* A newer build of nexus-server taking over from the running one:         *)
(* src/server/takeover.h (Handover, the installer's side, over nextStep;   *)
(* Drain, the running server's), as `nexus-server setup` and `rebuild`     *)
(* drive them through the detached `nexus-server handover`.                *)
(*                                                                         *)
(* setup places a build (Place) only when no handover runs, keeping the    *)
(* build placed before it as the previous one, and starts the handover     *)
(* (handing). The handover looks at the server and takes the step it       *)
(* gives (Ask): a server from before takeover (OldBuild, which answers     *)
(* neither `build` nor `takeover`) is stopped; any other server running a  *)
(* build other than the placed one is asked to drain. A draining server    *)
(* stops at the first moment no turn is open, or, once its deadline has    *)
(* passed (Late), with a turn open, cutting it. When no server runs, the   *)
(* placed build is started by the service, when there is one, or by the   *)
(* handover itself (Start). A build in Broken never comes up on the store  *)
(* (StartFails: it crashes, or never answers, however often it is         *)
(* started); the handover notices (failed) and puts the previous build     *)
(* back (Rollback), which is then started like any placed build. The       *)
(* handover ends when the placed build answers (HandedOver). Friends'      *)
(* turns open and end on whatever server runs (TurnOpen, TurnEnd).         *)
(*                                                                         *)
(* A build that does not even start on an empty home is refused before it  *)
(* is placed (setup's probe), so it never enters the model.                *)
(*                                                                         *)
(* State:                                                                  *)
(*   placed    the build at ~/.nexus/bin                                   *)
(*   previous  the build placed before it, which ran                       *)
(*   server    the running server's build, None when none runs             *)
(*   draining  whether it was asked to drain                               *)
(*   late      whether its drain's deadline has passed                     *)
(*   turn      whether a friend's turn is open on it                       *)
(*   places    how many builds were placed (bounds the model)              *)
(*   service   whether a per-user service restarts the server (fixed)      *)
(*   handing   whether a handover process runs                             *)
(*   failed    whether it saw the placed build fail to come up             *)
(*   rolled    whether it rolled back                                      *)
(*                                                                         *)
(* Properties:                                                             *)
(*   DrainCutsOnlyLate  a draining server never stops with a turn open     *)
(*                      before its deadline                                *)
(*   StopOnlyOld        a server is stopped outright (without draining)    *)
(*                      only when it is from before takeover               *)
(*   StartsPlaced       what starts is the placed build                    *)
(*   NeverRunsBroken    a broken build never runs                          *)
(*   IdlePlacedWorks    with no handover running, the placed build is one  *)
(*                      that comes up                                      *)
(*   WorkingRuns        after any placement, a build that works comes to   *)
(*                      run, the handover ends, and it stays so: with a    *)
(*                      service or without one                             *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    Builds,     \* the builds a rebuild places, each answering `build`
    Broken,     \* those of them that never come up on the store
    OldBuild,   \* a build from before takeover: it answers neither `build` nor `takeover`
    None,       \* no server runs
    MaxPlaces   \* how many builds the installer places, at most

ASSUME OldBuild \notin Builds /\ None \notin Builds /\ None # OldBuild /\ Broken \subseteq Builds

VARIABLES placed, previous, server, draining, late, turn, places, service, handing, failed, rolled

vars == <<placed, previous, server, draining, late, turn, places, service, handing, failed, rolled>>

AllBuilds == Builds \cup {OldBuild}

TypeOK ==
    /\ placed \in AllBuilds
    /\ previous \in AllBuilds
    /\ server \in AllBuilds \cup {None}
    /\ draining \in BOOLEAN
    /\ late \in BOOLEAN
    /\ turn \in BOOLEAN
    /\ places \in 0..MaxPlaces
    /\ service \in BOOLEAN
    /\ handing \in BOOLEAN
    /\ failed \in BOOLEAN
    /\ rolled \in BOOLEAN
    /\ (server = None) => (~draining /\ ~late /\ ~turn)
    /\ late => draining
    /\ (failed \/ rolled) => handing

\* An old install: the old build placed, and running or not; with a service or without one.
Init ==
    /\ placed = OldBuild
    /\ previous = OldBuild
    /\ server \in {OldBuild, None}
    /\ draining = FALSE
    /\ late = FALSE
    /\ turn = FALSE
    /\ places = 0
    /\ service \in BOOLEAN
    /\ handing = FALSE
    /\ failed = FALSE
    /\ rolled = FALSE

\* setup or rebuild places a build other than the one placed, keeps that one as the previous, and starts the handover.
Place(b) ==
    /\ ~handing
    /\ places < MaxPlaces
    /\ b # placed
    /\ previous' = placed
    /\ placed' = b
    /\ places' = places + 1
    /\ handing' = TRUE
    /\ UNCHANGED <<server, draining, late, turn, service, failed, rolled>>

\* The server stops: its turn, if open, is cut (the next server cuts it with a notice: models/Supervisor.tla).
Gone ==
    /\ server' = None
    /\ draining' = FALSE
    /\ late' = FALSE
    /\ turn' = FALSE

\* The handover's AskStop and AskDrain: it acts on a server running a build other than the placed one.
Ask ==
    /\ handing
    /\ server # None
    /\ server # placed
    /\ IF server = OldBuild
       THEN /\ Gone
       ELSE /\ ~draining
            /\ draining' = TRUE
            /\ UNCHANGED <<server, late, turn>>
    /\ UNCHANGED <<placed, previous, places, service, handing, failed, rolled>>

\* Drain::late: the deadline passes.
Late ==
    /\ server # None
    /\ draining
    /\ ~late
    /\ late' = TRUE
    /\ UNCHANGED <<placed, previous, server, draining, turn, places, service, handing, failed, rolled>>

\* Drain::stopNow: no turn open, or past the deadline.
DrainStop ==
    /\ server # None
    /\ draining
    /\ ~turn \/ late
    /\ Gone
    /\ UNCHANGED <<placed, previous, places, service, handing, failed, rolled>>

\* Who starts a server when none runs: the service, when there is one, and the handover while it runs.
Starter == service \/ handing

\* The placed build starts and answers.
Start ==
    /\ server = None
    /\ Starter
    /\ placed \notin Broken
    /\ server' = placed
    /\ UNCHANGED <<placed, previous, draining, late, turn, places, service, handing, failed, rolled>>

\* The placed build is started and does not come up; the handover's start deadline passes (Handover: failed).
StartFails ==
    /\ server = None
    /\ handing
    /\ placed \in Broken
    /\ ~failed
    /\ failed' = TRUE
    /\ UNCHANGED <<placed, previous, server, draining, late, turn, places, service, handing, rolled>>

\* Handover's RollBack: the previous program (and its driver) back in place, to be started like any placed build.
Rollback ==
    /\ handing
    /\ failed
    /\ ~rolled
    /\ server = None
    /\ placed' = previous
    /\ failed' = FALSE
    /\ rolled' = TRUE
    /\ UNCHANGED <<previous, server, draining, late, turn, places, service, handing>>

\* Handover's Done: the placed build answers, and the handover process ends.
HandedOver ==
    /\ handing
    /\ server = placed
    /\ handing' = FALSE
    /\ rolled' = FALSE
    /\ UNCHANGED <<placed, previous, server, draining, late, turn, places, service, failed>>

TurnOpen ==
    /\ server # None
    /\ ~turn
    /\ turn' = TRUE
    /\ UNCHANGED <<placed, previous, server, draining, late, places, service, handing, failed, rolled>>

TurnEnd ==
    /\ turn
    /\ turn' = FALSE
    /\ UNCHANGED <<placed, previous, server, draining, late, places, service, handing, failed, rolled>>

Next ==
    \/ \E b \in Builds : Place(b)
    \/ Ask
    \/ Late
    \/ DrainStop
    \/ Start
    \/ StartFails
    \/ Rollback
    \/ HandedOver
    \/ TurnOpen
    \/ TurnEnd

\* Turns may keep opening forever: no fairness on TurnOpen or TurnEnd. The handover, the deadline, the drain and the
\* starters are each sure to act. Nothing assumes a service: Start's fairness holds whenever the handover runs.
Fairness ==
    /\ WF_vars(Ask) /\ WF_vars(Late) /\ WF_vars(DrainStop) /\ WF_vars(Start) /\ WF_vars(StartFails)
    /\ WF_vars(Rollback) /\ WF_vars(HandedOver)

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

NeverRunsBroken == server \notin Broken

\* With no handover running, the placed build is one that comes up: what a service would restart.
IdlePlacedWorks == ~handing => placed \notin Broken

\* After any placement, a working build comes to run and the handover ends, and it stays so, with or without a service.
WorkingRuns ==
    <>[](places > 0 => (server = placed /\ server \notin Broken /\ ~handing))

=============================================================================
