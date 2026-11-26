-------------------------- MODULE RelayLifeTrace --------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real relay lifecycle *)
(* (src/relay/relay_life.h: RelayLife, driven as src/relay/relay_port_win.cpp *)
(* drives it) is a behaviour of RelayLife.tla.                             *)
(*                                                                         *)
(* tests/relay/relay_walk.cpp drives the code with a seeded random walk    *)
(* and writes RelayLifeTraceData.tla beside this module. It defines        *)
(*   TraceMaxRelays, TraceMaxActions: the instance, and                    *)
(*   Trace: one record per step of the walk, the state after it: phase,     *)
(*     gen, left, backoff, sent, written, base, ackHigh and cut read from   *)
(*     the code (RelayLife's), told the actions cut its polls returned,    *)
(*     and procs, wr and ap from the walk's world: the relay processes, and *)
(*     each action's relays and the server's applications of it, found by  *)
(*     the action's own text (the window's number), not by the code's      *)
(*     bookkeeping. The relays are real RelayServer ends, reading the bytes *)
(*     RelayLife hands the port; their state frames are real StateWriter    *)
(*     frames carrying the server's first unapplied action.                *)
(*                                                                         *)
(* Each record after the first must be one step of RelayLife's Next from    *)
(* the one before, or no change at all. Every safety property of RelayLife  *)
(* is checked on the trace.                                                *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, RelayLifeTraceData

VARIABLES phase, procs, gen, left, backoff, sent, written, base, ackHigh, cut, told, wr, ap, l

R == INSTANCE RelayLife WITH MaxRelays <- TraceMaxRelays, MaxActions <- TraceMaxActions, First <- 1, Last <- 10

specVars == <<phase, procs, gen, left, backoff, sent, written, base, ackHigh, cut, told, wr, ap>>
traceVars == <<phase, procs, gen, left, backoff, sent, written, base, ackHigh, cut, told, wr, ap, l>>

\* A record's wr or ap: a sequence of sets, one per action, as the function the spec holds.
Fn(s) == [n \in 1 .. TraceMaxActions |-> s[n]]

Is(k) ==
    LET s == Trace[k] IN
    /\ phase = s.phase
    /\ procs = s.procs
    /\ gen = s.gen
    /\ left = s.left
    /\ backoff = s.backoff
    /\ sent = s.sent
    /\ written = s.written
    /\ base = s.base
    /\ ackHigh = s.ackHigh
    /\ cut = s.cut
    /\ told = s.told
    /\ wr = Fn(s.wr)
    /\ ap = Fn(s.ap)

TraceMatches == Is(l)

Becomes(k) ==
    LET s == Trace[k] IN
    /\ phase' = s.phase
    /\ procs' = s.procs
    /\ gen' = s.gen
    /\ left' = s.left
    /\ backoff' = s.backoff
    /\ sent' = s.sent
    /\ written' = s.written
    /\ base' = s.base
    /\ ackHigh' = s.ackHigh
    /\ cut' = s.cut
    /\ told' = s.told
    /\ wr' = Fn(s.wr)
    /\ ap' = Fn(s.ap)

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
OneRelay == R!OneRelay
AppliedOnce == R!AppliedOnce
AckTrue == R!AckTrue
Covered == R!Covered
BackoffOK == R!BackoffOK
CutOnlyUnacked == R!CutOnlyUnacked
WaitsBackoff == R!WaitsBackoff

=============================================================================
