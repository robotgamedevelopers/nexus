------------------------- MODULE AccountSwitchTrace -------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real WatchLink and   *)
(* PhoneRelay switching an AI's account (watch/core/relay.h) is a           *)
(* behaviour of AccountSwitch.tla.                                          *)
(*                                                                         *)
(* watch/tests/relay_walk.cpp, asked for AccountSwitchTraceData.tla, walks  *)
(* the account switch and writes it beside this module: its bounds and     *)
(* Trace, one record per event: what it was (act), the account chosen (a),  *)
(* the frame's choice (r), the answer (m) or the SetAccount delivered (x)   *)
(* it names, and the code's state after it (s). TraceMatches requires the   *)
(* spec's state to equal the code's after every event. Deadlock checking    *)
(* stays on: a step the spec cannot take is a deadlock.                     *)
(***************************************************************************)
EXTENDS Integers, Sequences, AccountSwitchTraceData

VARIABLES server, applied, chosen, choice, shown, reqs, resps, seen, waiting, inflight, runQ, switched, view, linkQ,
          crashes, l

S == INSTANCE AccountSwitch WITH Accounts <- TraceAccounts, SignedIn <- TraceSignedIn, Start <- TraceStart,
                                 MaxChoice <- TraceMaxChoice, MaxCrash <- TraceMaxCrash, MaxMsgs <- TraceMaxMsgs

traceVars == <<server, applied, chosen, choice, shown, reqs, resps, seen, waiting, inflight, runQ, switched, view,
               linkQ, crashes, l>>

Matches(k) ==
    LET c == Trace[k].s IN
    /\ server = c.server /\ applied = c.applied /\ chosen = c.chosen /\ choice = c.choice /\ shown = c.shown
    /\ reqs = c.reqs /\ resps = c.resps
    /\ seen = c.seen /\ waiting = c.waiting /\ inflight = c.inflight /\ runQ = c.runQ /\ switched = c.switched
    /\ view = c.view /\ linkQ = c.linkQ /\ crashes = c.crashes

TraceMatches == Matches(l)

TraceInit == l = 1 /\ S!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ LET e == Trace[l + 1] IN
       CASE e.act = "choose" -> S!Choose(e.a)
         [] e.act = "send" -> S!Send /\ e.r = choice
         [] e.act = "receive" -> e.r \in reqs /\ S!Receive(e.r)
         [] e.act = "hand" -> S!Hand
         [] e.act = "look" -> S!Look
         [] e.act = "deliver" -> S!Deliver(e.x)
         [] e.act = "ack" -> S!Ack
         [] e.act = "take" -> e.m \in resps /\ S!Take(e.m)
         [] e.act = "roundtrip" -> S!RoundTrip
         [] e.act = "crash" -> S!Crash
         [] e.act = "losereq" -> e.r \in reqs /\ S!LoseReq(e.r)
         [] e.act = "loseresp" -> e.m \in resps /\ S!LoseResp(e.m)

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == S!TypeOK
OnlySignedIn == S!OnlySignedIn
NoSilentDrop == S!NoSilentDrop
=============================================================================
