-------------------------- MODULE VoiceTurnTrace --------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real                 *)
(* nexus::watch::VoiceTurn is a behaviour of VoiceTurn.tla.                *)
(*                                                                         *)
(* watch/tests/voice_walk.cpp drives the code with a seeded random walk    *)
(* and writes VoiceTurnTraceData.tla beside this module: TraceAis, the      *)
(* AIs it walks with, and Trace, one record per event: what it was      *)
(* (act), its arguments (b: the microphone allowed, the hold long enough or *)
(* the deadline passed; n: a recording or prompt number; route and f: what  *)
(* the matcher made of the words, and the AI it named), what the code   *)
(* returned (ok), and the code's state after it (s).                        *)
(*                                                                         *)
(* Each step takes the record's event in VoiceTurn.tla with the record's    *)
(* arguments. TraceMatches requires the spec's state and return to equal    *)
(* the code's after every event, and the properties are checked on the      *)
(* trace as well. Deadlock checking stays on: a step the spec cannot take   *)
(* is a deadlock.                                                          *)
(***************************************************************************)
EXTENDS Integers, Sequences, VoiceTurnTraceData

VARIABLES phase, connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues, last,
          unlocked, l

V == INSTANCE VoiceTurn WITH Ais <- TraceAis, MaxPrompts <- 100000, MaxUtter <- 100000,
                             MaxReplies <- 100000, MaxSaid <- 100000

traceVars == <<phase, connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues,
               last, unlocked, l>>

Matches(k) ==
    LET c == Trace[k].s IN
    /\ phase = c.phase /\ connected = c.connected /\ current = c.current
    /\ utterance = c.utterance /\ heard = c.heard /\ unapplied = c.unapplied /\ nextPrompt = c.nextPrompt
    /\ awaiting = c.awaiting /\ replies = c.replies /\ notices = c.notices /\ cues = c.cues
    /\ unlocked = c.unlocked
    /\ (k > 1 => last.ok = Trace[k].ok)

TraceMatches == Matches(l)

TraceInit == l = 1 /\ V!Init

Quiet(e) ==
    /\ UNCHANGED <<cues, unlocked>>
    /\ CASE e.act = "press" -> V!Press(e.b)
         [] e.act = "release" -> V!Release(e.b)
         [] e.act = "transcript" -> V!Transcript(e.n)
         [] e.act = "failed" -> V!TranscriptFailed(e.n)
         [] e.act = "applied" -> V!Applied(e.n)
         [] e.act = "lostprompt" -> V!LostPrompt(e.n)
         [] e.act = "reply" -> V!Reply
         [] e.act = "spoken" -> V!SpeechDone
         [] e.act = "resume" -> V!Resume
         [] e.act = "dismiss" -> V!Dismiss
         [] e.act = "lost" -> V!Lost
         [] e.act = "tick" -> V!Tick(e.b)

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ LET e == Trace[l + 1] IN
       CASE e.act = "choose" -> V!Choose(e.route, e.f) /\ UNCHANGED unlocked
         [] e.act = "back" -> V!Back /\ UNCHANGED unlocked
         [] e.act = "lock" -> V!Lock
         [] e.act = "unlock" -> V!Unlock
         [] OTHER -> Quiet(e)

\* The end of the trace: stutter, so that a trace read to its end is no deadlock.
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == V!TypeOK
TalkOnlyConnected == V!TalkOnlyConnected
DisconnectedExactly == V!DisconnectedExactly
SendingExactly == V!SendingExactly
PromptsNumbered == V!PromptsNumbered
SpeakingHasReply == V!SpeakingHasReply
NoReplyLeftUnread == V!NoReplyLeftUnread
LockedQuiet == V!LockedQuiet
PressRefusedSays == V!PressRefusedSays
LostSays == V!LostSays
TurnEndsSaid == V!TurnEndsSaid
PromptConfirmed == V!PromptConfirmed
PromptsOnlyChosen == V!PromptsOnlyChosen
PromptsNeverDropped == V!PromptsNeverDropped
RefusedChangesNothing == V!RefusedChangesNothing
=============================================================================
