----------------------------- MODULE LinkTrace -----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real link code is a  *)
(* behaviour of Link.tla.                                                  *)
(*                                                                         *)
(* tests/sync/link_walk.cpp drives the code (sync::Actions, Applier, Hub,  *)
(* Sessions and Mirror, and nexus::Model as the windows) through a copy of *)
(* mosh's bookkeeping (tests/sync/fake_mosh.h) under loss, duplication and *)
(* reordering from a seeded generator, with the server's own steps (an     *)
(* open entry growing and closing, a queued prompt taken, a restart), and  *)
(* writes LinkTraceData.tla beside this module. It defines                 *)
(*   TraceClients, TraceAIs, TraceStart, TraceMaxSessions,             *)
(*     TraceMaxActions, TraceW, TraceMaxGrows, TraceMaxRestarts: the       *)
(*     instance, and                                                       *)
(*   Trace: one record per step. Trace[1] is the start (op "init"); each   *)
(*     later record is the step the code took (a window's action, a range *)
(*     of actions reaching the server's Applier, a window receiving one of *)
(*     the server's states, or one of the server's own steps), its         *)
(*     arguments, and the code's whole state after it: each window's list, *)
(*     the Applier's count, the server's sessions and its next id, their   *)
(*     transcripts and queues (its store, whole), its open entries (open,  *)
(*     as its state shows), the stops that reached each session's          *)
(*     responder or were refused, how many open entries, grows and         *)
(*     restarts it has made, how many states it has made, the state each   *)
(*     window's receiver holds, and each window's session shown per AI *)
(*     tab, transcripts, queues, mirror and open entries.                  *)
(*                                                                         *)
(* Each step of this spec takes the Link action the record names. The      *)
(* invariant TraceMatches requires the spec's state to equal the code's.   *)
(* A step the spec cannot take is a deadlock. Link's safety properties are *)
(* checked on the trace, and the walk ends by letting mosh deliver         *)
(* everything, so the trace's last state must be Link's Settled.           *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, LinkTraceData

VARIABLES acts, applied, done, ses, nextSid, minted, skipped, tr, q, op, stops, made, snaps, last, shown, view, vq,
          seen, live, l

L == INSTANCE Link WITH Clients <- TraceClients, AIs <- TraceAIs, Start <- TraceStart,
                        MaxSessions <- TraceMaxSessions, MaxActions <- TraceMaxActions, W <- TraceW,
                        MaxGrows <- TraceMaxGrows, MaxRestarts <- TraceMaxRestarts,
                        Kinds <- {"Send", "Queue", "Stop", "Create", "Ensure"}

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET s == Trace[k] IN
    /\ acts = s.acts
    /\ applied = s.applied
    /\ ses = s.ses
    /\ nextSid = s.nextSid
    /\ tr = s.tr
    /\ q = s.q
    /\ op = s.open
    /\ stops = s.stops
    /\ made = s.made
    /\ Len(snaps) = s.states
    /\ last = s.last
    /\ shown = s.shown
    /\ view = s.view
    /\ vq = s.vq
    /\ seen = s.seen
    /\ live = s.live

TraceMatches == Matches(l)

Step(s) ==
    CASE s.op = "act"     -> L!Act(s.c, s.a)
      [] s.op = "deliver" -> L!Deliver(s.c, s.first, s.lst)
      [] s.op = "receive" -> L!Receive(s.c, s.j)
      [] s.op = "grow"    -> L!Grow(s.t)
      [] s.op = "close"   -> L!Close(s.t)
      [] s.op = "take"    -> L!Take(s.t)
      [] s.op = "restart" -> L!Restart
      [] OTHER            -> FALSE

traceVars == <<acts, applied, done, ses, nextSid, minted, skipped, tr, q, op, stops, made, snaps, last, shown, view,
               vq, seen, live, l>>

TraceInit == l = 1 /\ L!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* Link's spec and properties, checked on the trace.
LinkSpec == L!SafetySpec
TypeOK == L!TypeOK
ExactlyOnce == L!ExactlyOnce
IdsNeverReused == L!IdsNeverReused
SessionsKept == L!SessionsKept
EnsureMakesOne == L!EnsureMakesOne
ShowsFirst == L!ShowsFirst
ServerHoldsEach == L!ServerHoldsEach
ViewMatches == L!ViewMatches
QueueMatches == L!QueueMatches
NoticesSound == L!NoticesSound
EachClosedOnce == L!EachClosedOnce
LiveSound == L!LiveSound
OnlyGrow == L!OnlyGrow
OpenGrows == L!OpenGrows
EndsSettled == l = Len(Trace) => L!Settled

=============================================================================
