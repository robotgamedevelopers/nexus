-------------------------- MODULE ReconnectTrace --------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real reconnect       *)
(* decision (src/sync/redial.h: Redial, as src/link/client.cpp drives it)  *)
(* is a behaviour of Reconnect.tla.                                        *)
(*                                                                         *)
(* tests/sync/reconnect_walk.cpp drives the code with a seeded random walk *)
(* and writes ReconnectTraceData.tla beside this module. It defines        *)
(*   TraceMaxServers: the instance, and                                    *)
(*   Trace: one record per step of the walk, the state after it: srv,      *)
(*     started, link and heard from the walk's world (the servers, and     *)
(*     the session its bootstrap made), and quiet, ended and due read from *)
(*     the code (Redial::quiet, serverEnded and timerDue); gen flips at    *)
(*     each session made, and the walk runs the bootstrap only when        *)
(*     Redial::due says so.                                                *)
(*                                                                         *)
(* Each record after the first must be one step of Reconnect's Next from   *)
(* the one before, or no change at all. Every safety property of           *)
(* Reconnect is checked on the trace.                                      *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, ReconnectTraceData

VARIABLES srv, started, link, heard, quiet, ended, due, gen, l

R == INSTANCE Reconnect WITH MaxServers <- TraceMaxServers

specVars == <<srv, started, link, heard, quiet, ended, due, gen>>
traceVars == <<srv, started, link, heard, quiet, ended, due, gen, l>>

Is(k) ==
    LET s == Trace[k] IN
    /\ srv = s.srv
    /\ started = s.started
    /\ link = s.link
    /\ heard = s.heard
    /\ quiet = s.quiet
    /\ ended = s.ended
    /\ due = s.due
    /\ gen = s.gen

TraceMatches == Is(l)

\* the spec's next state is record k
Becomes(k) ==
    LET s == Trace[k] IN
    /\ srv' = s.srv
    /\ started' = s.started
    /\ link' = s.link
    /\ heard' = s.heard
    /\ quiet' = s.quiet
    /\ ended' = s.ended
    /\ due' = s.due
    /\ gen' = s.gen

TraceInit == l = 1 /\ R!Init /\ Is(1)

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ R!Next \/ UNCHANGED specVars
    /\ Becomes(l + 1)

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == R!TypeOK
DialsLive == R!DialsLive
NeverBack == R!NeverBack

=============================================================================
