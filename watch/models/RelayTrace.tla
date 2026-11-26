---------------------------- MODULE RelayTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real WatchLink and   *)
(* PhoneRelay (watch/core/relay.h) is a behaviour of Relay.tla.            *)
(*                                                                         *)
(* watch/tests/relay_walk.cpp writes RelayTraceData.tla beside this module: *)
(* its bounds (TraceN, TraceMaxCrash, TraceMaxMsgs) and Trace, one record   *)
(* per event: what it was (act), the frame (r), answer (m) or prompt (n) it *)
(* names, and the code's state after it (s). Each step takes the event in   *)
(* Relay.tla; TraceMatches requires the spec's state to equal the code's   *)
(* after every event, and the invariants are checked on the trace too.      *)
(* Deadlock checking stays on: a step the spec cannot take is a deadlock.   *)
(***************************************************************************)
EXTENDS Integers, Sequences, RelayTraceData

VARIABLES made, outbox, told, dropped, reqs, resps, handed, boot, acked, waiting, inflight, linkQ, applied, crashes, l

R == INSTANCE Relay WITH N <- TraceN, MaxCrash <- TraceMaxCrash, MaxMsgs <- TraceMaxMsgs

traceVars == <<made, outbox, told, dropped, reqs, resps, handed, boot, acked, waiting, inflight, linkQ, applied,
               crashes, l>>

Matches(k) ==
    LET c == Trace[k].s IN
    /\ made = c.made /\ outbox = c.outbox /\ told = c.told /\ dropped = c.dropped
    /\ reqs = c.reqs /\ resps = c.resps
    /\ handed = c.handed /\ boot = c.boot /\ acked = c.acked /\ waiting = c.waiting /\ inflight = c.inflight
    /\ linkQ = c.linkQ /\ applied = c.applied /\ crashes = c.crashes

TraceMatches == Matches(l)

TraceInit == l = 1 /\ R!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ LET e == Trace[l + 1] IN
       CASE e.act = "make" -> R!Make
         [] e.act = "send" -> R!Send /\ e.r = outbox
         [] e.act = "receive" -> e.r \in reqs /\ R!Receive(e.r)
         [] e.act = "hand" -> R!Hand
         [] e.act = "deliver" -> R!Deliver(e.n)
         [] e.act = "ack" -> R!Ack
         [] e.act = "take" -> e.m \in resps /\ R!Take(e.m)
         [] e.act = "roundtrip" -> R!RoundTrip
         [] e.act = "crash" -> R!Crash
         [] e.act = "losereq" -> e.r \in reqs /\ R!LoseReq(e.r)
         [] e.act = "loseresp" -> e.m \in resps /\ R!LoseResp(e.m)

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == R!TypeOK
AtMostOnce == R!AtMostOnce
NoSilentLoss == R!NoSilentLoss
DroppedWasApplied == R!DroppedWasApplied
HandedOnce == R!HandedOnce
=============================================================================
