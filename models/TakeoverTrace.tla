--------------------------- MODULE TakeoverTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real takeover code   *)
(* (src/server/takeover.h: nextStep and Drain) is a behaviour of           *)
(* Takeover.tla.                                                           *)
(*                                                                         *)
(* tests/server/takeover_walk.cpp drives the code with a seeded random     *)
(* walk and writes TakeoverTraceData.tla beside this module. It defines    *)
(*   TraceBuilds, TraceOld, TraceNone, TraceMaxPlaces: the instance, and   *)
(*   Trace: one record per step of the walk, the state after it: placed,   *)
(*     server, draining, late, turn and places, each read from the code    *)
(*     (the server's Drain, the installer's nextStep and what it did).     *)
(*                                                                         *)
(* Each record after the first must be one step of Takeover's Next from    *)
(* the one before, or no change at all (a step that changed nothing: the   *)
(* installer waiting, the clock moving short of the deadline). Every       *)
(* Takeover safety property is checked on the trace.                       *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, TakeoverTraceData

VARIABLES placed, server, draining, late, turn, places, l

T == INSTANCE Takeover WITH Builds <- TraceBuilds, OldBuild <- TraceOld, None <- TraceNone,
                            MaxPlaces <- TraceMaxPlaces

specVars == <<placed, server, draining, late, turn, places>>
traceVars == <<placed, server, draining, late, turn, places, l>>

Is(k) ==
    LET s == Trace[k] IN
    /\ placed = s.placed
    /\ server = s.server
    /\ draining = s.draining
    /\ late = s.late
    /\ turn = s.turn
    /\ places = s.places

TraceMatches == Is(l)

\* the spec's next state is record k
Becomes(k) ==
    LET s == Trace[k] IN
    /\ placed' = s.placed
    /\ server' = s.server
    /\ draining' = s.draining
    /\ late' = s.late
    /\ turn' = s.turn
    /\ places' = s.places

TraceInit == l = 1 /\ T!Init /\ Is(1)

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ T!Next \/ UNCHANGED specVars
    /\ Becomes(l + 1)

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == T!TypeOK
DrainCutsOnlyLate == T!DrainCutsOnlyLate
StopOnlyOld == T!StopOnlyOld
StartsPlaced == T!StartsPlaced

=============================================================================
