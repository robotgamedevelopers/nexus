--------------------------- MODULE VoiceTurnTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real                 *)
(* nexus::voice::VoiceTurn is a behaviour of VoiceTurn.tla.                *)
(*                                                                         *)
(* tests/voice/voice_walk.cpp drives the code with a seeded random walk    *)
(* and writes VoiceTurnTraceData.tla beside this module. It defines        *)
(*   TraceAIs: the AIs it walks, and                               *)
(*   Trace: one record per step: its operation and arguments (the AI,  *)
(*     the OS's answer, whether the forming transcript is empty, the       *)
(*     routing of a final one by the real matcher and the AI showing,  *)
(*     the reply mode chosen, and for a tick whether it stopped a listen   *)
(*     or finished one), the acts the code returned, and its state after.  *)
(*                                                                         *)
(* Each step takes the spec's action with the record's arguments; the      *)
(* invariant TraceMatches requires the spec's state and acts to equal the  *)
(* code's. Every safety property of VoiceTurn is checked on the trace too. *)
(***************************************************************************)
EXTENDS Naturals, Sequences, TLC, VoiceTurnTraceData

VARIABLES phase, permission, reply, speaking, queue, awaiting, heard, last, l

V == INSTANCE VoiceTurn WITH AIs <- TraceAIs, None <- "none"

\* the spec's state, and the acts of the step that reached it, equal what the code recorded at step k
Matches(k) ==
    LET s == Trace[k] IN
    /\ phase = s.code.phase
    /\ permission = s.code.permission
    /\ reply = s.code.reply
    /\ speaking = s.code.speaking
    /\ queue = s.code.queue
    /\ awaiting = s.code.awaiting
    /\ heard = s.code.heard
    /\ last.acts = s.acts

TraceMatches == Matches(l)

traceVars == <<phase, permission, reply, speaking, queue, awaiting, heard, last, l>>

TraceInit == l = 1 /\ V!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ LET s == Trace[l + 1] IN
       CASE s.op = "press"      -> V!Press
         [] s.op = "permission" -> V!Permission(s.granted)
         [] s.op = "partial"    -> V!Partial(s.h)
         [] s.op = "final"      -> V!Final(s.out, s.cur)
         [] s.op = "fail"       -> V!Fail
         [] s.op = "tick" /\ s.tick = "stop" -> V!TickStop
         [] s.op = "tick"       -> V!TickFinish(s.out, s.cur)
         [] s.op = "reply"      -> V!Reply(s.ai)
         [] s.op = "spoken"     -> V!Spoken
         [] s.op = "turn_end"   -> V!TurnEnd(s.ai)
         [] s.op = "set_reply"  -> V!SetReply(s.r)

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == V!TypeOK
NeverHearsItself == V!NeverHearsItself
TextIsSilent == V!TextIsSilent
DeniedNeverListens == V!DeniedNeverListens
NoStall == V!NoStall
SpeaksAnswers == V!SpeaksAnswers
ListenStopsSpeech == V!ListenStopsSpeech
SentOnce == V!SentOnce
NothingLost == V!NothingLost
DeniedIsDenied == V!DeniedIsDenied

=============================================================================
