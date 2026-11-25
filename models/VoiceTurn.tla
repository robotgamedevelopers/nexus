------------------------------ MODULE VoiceTurn ------------------------------
(***************************************************************************)
(* One voice turn of a client: nexus::voice::VoiceTurn in                  *)
(* src/voice/voice_turn.h. The microphone button, the OS's permission, the *)
(* recognizer's transcript as it forms and at its end, which AI it     *)
(* goes to, and the replies spoken back.                                   *)
(*                                                                         *)
(* Which AI is named is the matcher's (src/voice/name_match.h, pure,   *)
(* unit tested): here a final transcript comes with its routing, `out`:    *)
(* an AI named with or without a prompt after the name, the AI     *)
(* showing with or without a prompt (without: silence), or ambiguous. The  *)
(* time is abstract: a Tick may end a listen or take a finishing one at    *)
(* what it heard whenever the code's deadlines would.                      *)
(*                                                                         *)
(* State, as the code's members:                                           *)
(*   phase       idle, asking (the OS), listening, finishing (stopped,     *)
(*               the recognizer's final result to come), denied            *)
(*   permission  unknown, granted, refused                                 *)
(*   reply       text, voice or both: the person's choice                  *)
(*   speaking    the synthesizer is speaking a reply                       *)
(*   queue       the AIs whose replies wait to be spoken, in order     *)
(*   awaiting    the AIs sent a prompt by voice whose turn is open     *)
(*   heard       the transcript is not empty                               *)
(* `last` is a history variable: the step's operation, its AI, and the *)
(* acts it returned.                                                       *)
(***************************************************************************)
EXTENDS Naturals, Sequences, TLC

CONSTANTS AIs, None

ASSUME None \notin AIs

Phases == {"idle", "asking", "listening", "finishing", "denied"}
Replies == {"text", "voice", "both"}
Outs == [route : {"named"}, ai : AIs, prompt : BOOLEAN]
     \cup [route : {"current", "ambiguous"}, ai : {None}, prompt : BOOLEAN]
Ops == {"init", "press", "permission", "partial", "final", "fail", "tick", "reply", "spoken", "turn_end", "set_reply"}

NoActs == [ask |-> FALSE, start |-> FALSE, stopListen |-> FALSE, stopSpeak |-> FALSE, speak |-> None,
           open |-> None, send |-> None, draft |-> FALSE]
Acts == [ask : BOOLEAN, start : BOOLEAN, stopListen : BOOLEAN, stopSpeak : BOOLEAN, speak : AIs \cup {None},
         open : AIs \cup {None}, send : AIs \cup {None}, draft : BOOLEAN]

VARIABLES phase, permission, reply, speaking, queue, awaiting, heard, last

vars == <<phase, permission, reply, speaking, queue, awaiting, heard, last>>

TypeOK ==
    /\ phase \in Phases
    /\ permission \in {"unknown", "granted", "refused"}
    /\ reply \in Replies
    /\ speaking \in BOOLEAN
    /\ queue \in Seq(AIs)
    /\ awaiting \subseteq AIs
    /\ heard \in BOOLEAN
    /\ last \in [op : Ops, ai : AIs \cup {None}, acts : Acts]

Init ==
    /\ phase = "idle"
    /\ permission = "unknown"
    /\ reply = "both"
    /\ speaking = FALSE
    /\ queue = <<>>
    /\ awaiting = {}
    /\ heard = FALSE
    /\ last = [op |-> "init", ai |-> None, acts |-> NoActs]

Listening == phase \in {"listening", "finishing"}
\* The microphone is pending or on: no reply is spoken then (speakNext).
MicBusy == phase \in {"asking", "listening", "finishing"}

\* speakNext, after a step that leaves phase p, speaking s and queue q: the next reply spoken, if any.
SpeakNext(p, s, q) == IF ~s /\ q # <<>> /\ p \notin {"asking", "listening", "finishing"} THEN Head(q) ELSE None

\* A step that ends with phase p and queue q (before speakNext), the speech still s, and acts a (before speakNext).
Ends(p, s, q, a, op, f) ==
    LET next == SpeakNext(p, s, q) IN
    /\ phase' = p
    /\ speaking' = (s \/ next # None)
    /\ queue' = IF next # None THEN Tail(q) ELSE q
    /\ last' = [op |-> op, ai |-> f, acts |-> [a EXCEPT !.speak = next]]

\* listen: the speech stops and its queue goes; the microphone starts.
Listen(op) ==
    /\ phase' = "listening"
    /\ speaking' = FALSE
    /\ queue' = <<>>
    /\ heard' = FALSE
    /\ last' = [op |-> op, ai |-> None, acts |-> [NoActs EXCEPT !.stopSpeak = speaking, !.start = TRUE]]

\* VoiceTurn::press
Press ==
    /\ \/ /\ phase \in {"idle", "denied"} /\ permission = "granted"
          /\ Listen("press")
          /\ UNCHANGED <<permission, reply, awaiting>>
       \/ /\ phase \in {"idle", "denied"} /\ permission # "granted"
          /\ phase' = "asking" /\ heard' = FALSE
          /\ last' = [op |-> "press", ai |-> None, acts |-> [NoActs EXCEPT !.ask = TRUE]]
          /\ UNCHANGED <<permission, reply, speaking, queue, awaiting>>
       \/ /\ phase = "listening"
          /\ phase' = "finishing"
          /\ last' = [op |-> "press", ai |-> None, acts |-> [NoActs EXCEPT !.stopListen = TRUE]]
          /\ UNCHANGED <<permission, reply, speaking, queue, awaiting, heard>>
       \/ /\ phase \in {"asking", "finishing"}
          /\ last' = [op |-> "press", ai |-> None, acts |-> NoActs]
          /\ UNCHANGED <<phase, permission, reply, speaking, queue, awaiting, heard>>

\* VoiceTurn::permission: an answer when nothing asked changes nothing
Permission(g) ==
    IF phase # "asking"
    THEN /\ last' = [op |-> "permission", ai |-> None, acts |-> NoActs]
         /\ UNCHANGED <<phase, permission, reply, speaking, queue, awaiting, heard>>
    ELSE IF g
    THEN /\ permission' = "granted"
         /\ Listen("permission")
         /\ UNCHANGED <<reply, awaiting>>
    ELSE /\ permission' = "refused"
         /\ Ends("denied", speaking, queue, NoActs, "permission", None)
         /\ UNCHANGED <<reply, awaiting, heard>>

\* VoiceTurn::heard(text, false): the transcript as it forms (h: it is not empty)
Partial(h) ==
    /\ heard' = IF Listening THEN h ELSE heard
    /\ last' = [op |-> "partial", ai |-> None, acts |-> NoActs]
    /\ UNCHANGED <<phase, permission, reply, speaking, queue, awaiting>>

\* An outcome is one the transcript can have: silence exactly when nothing was heard.
Silence(out) == out.route = "current" /\ ~out.prompt
Consistent(out, h) == (h = ~Silence(out)) /\ (out.route = "ambiguous" => out.prompt)

\* VoiceTurn::finish, with the routing of what was heard and the AI showing (cur, None for none)
Finish(out, cur, op) ==
    LET a0 == [NoActs EXCEPT !.stopListen = (phase = "listening")]
        a == CASE out.route = "named" /\ out.prompt -> [a0 EXCEPT !.open = out.ai, !.send = out.ai]
               [] out.route = "named"              -> [a0 EXCEPT !.open = out.ai]
               [] out.route = "ambiguous"          -> [a0 EXCEPT !.draft = TRUE]
               [] ~out.prompt                      -> a0
               [] cur = None                       -> [a0 EXCEPT !.draft = TRUE]
               [] OTHER                            -> [a0 EXCEPT !.send = cur]
    IN /\ awaiting' = IF a.send # None THEN awaiting \cup {a.send} ELSE awaiting
       /\ Ends("idle", speaking, queue, a, op, None)

\* VoiceTurn::heard(text, true)
Final(out, cur) ==
    IF ~Listening
    THEN /\ last' = [op |-> "final", ai |-> None, acts |-> NoActs]
         /\ UNCHANGED <<phase, permission, reply, speaking, queue, awaiting, heard>>
    ELSE /\ heard' = ~Silence(out)
         /\ Finish(out, cur, "final")
         /\ UNCHANGED <<permission, reply>>

\* VoiceTurn::failed: what was heard goes to the draft
Fail ==
    IF phase \notin {"asking", "listening", "finishing"}
    THEN /\ last' = [op |-> "fail", ai |-> None, acts |-> NoActs]
         /\ UNCHANGED <<phase, permission, reply, speaking, queue, awaiting, heard>>
    ELSE /\ Ends("idle", speaking, queue,
                 [NoActs EXCEPT !.stopListen = (phase = "listening"), !.draft = heard], "fail", None)
         /\ UNCHANGED <<permission, reply, awaiting, heard>>

\* VoiceTurn::tick past a deadline: a listen too long is stopped; a stopped one with no final result is taken at what
\* it heard (its routing `out`, consistent with heard).
TickStop ==
    /\ phase = "listening"
    /\ phase' = "finishing"
    /\ last' = [op |-> "tick", ai |-> None, acts |-> [NoActs EXCEPT !.stopListen = TRUE]]
    /\ UNCHANGED <<permission, reply, speaking, queue, awaiting, heard>>
TickFinish(out, cur) ==
    /\ phase = "finishing"
    /\ Consistent(out, heard)
    /\ Finish(out, cur, "tick")
    /\ UNCHANGED <<permission, reply, heard>>
Tick == TickStop \/ \E out \in Outs, cur \in AIs \cup {None} : TickFinish(out, cur)

\* VoiceTurn::replied: a block of f's reply closed
Reply(f) ==
    IF reply = "text" \/ f \notin awaiting
    THEN /\ last' = [op |-> "reply", ai |-> f, acts |-> NoActs]
         /\ UNCHANGED <<phase, permission, reply, speaking, queue, awaiting, heard>>
    ELSE /\ Ends(phase, speaking, Append(queue, f), NoActs, "reply", f)
         /\ UNCHANGED <<permission, reply, awaiting, heard>>

\* VoiceTurn::spoken
Spoken ==
    IF ~speaking
    THEN /\ last' = [op |-> "spoken", ai |-> None, acts |-> NoActs]
         /\ UNCHANGED <<phase, permission, reply, speaking, queue, awaiting, heard>>
    ELSE /\ Ends(phase, FALSE, queue, NoActs, "spoken", None)
         /\ UNCHANGED <<permission, reply, awaiting, heard>>

\* VoiceTurn::turnEnded
TurnEnd(f) ==
    /\ awaiting' = awaiting \ {f}
    /\ last' = [op |-> "turn_end", ai |-> f, acts |-> NoActs]
    /\ UNCHANGED <<phase, permission, reply, speaking, queue, heard>>

\* VoiceTurn::setReply
SetReply(r) ==
    /\ reply' = r
    /\ queue' = IF r = "text" THEN <<>> ELSE queue
    /\ speaking' = (speaking /\ r # "text")
    /\ last' = [op |-> "set_reply", ai |-> None, acts |-> [NoActs EXCEPT !.stopSpeak = (speaking /\ r = "text")]]
    /\ UNCHANGED <<phase, permission, awaiting, heard>>

Next ==
    \/ Press
    \/ \E g \in BOOLEAN : Permission(g)
    \/ \E h \in BOOLEAN : Partial(h)
    \/ \E out \in Outs, cur \in AIs \cup {None} : Final(out, cur)
    \/ Fail
    \/ Tick
    \/ \E f \in AIs : Reply(f)
    \/ Spoken
    \/ \E f \in AIs : TurnEnd(f)
    \/ \E r \in Replies : SetReply(r)

Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)

(***************************************************************************)
(* Properties, in the owner's terms.                                       *)
(***************************************************************************)

\* The microphone is never on while a reply is spoken: nexus never hears itself.
NeverHearsItself == Listening => ~speaking

\* With replies as text only, nothing is spoken or waits to be.
TextIsSilent == reply = "text" => (~speaking /\ queue = <<>>)

\* Without the microphone and speech recognition granted, nothing listens.
DeniedNeverListens == permission # "granted" => ~Listening

\* No reply waits unspoken while nothing is speaking and the microphone is free.
NoStall == queue # <<>> => (speaking \/ MicBusy)

\* Only an answer to a prompt sent by voice is spoken or queued.
SpeaksAnswers ==
    [][(last'.op = "reply" /\ (last'.ai \notin awaiting \/ reply = "text")) =>
        (queue' = queue /\ speaking' = speaking /\ last'.acts.speak = None)]_vars

\* Starting to listen stops the speech in the same step.
ListenStopsSpeech == [][last'.acts.start => (~speaking' /\ last'.acts.stopSpeak = speaking)]_vars

\* What was heard is sent once: only by the step that ends the listen.
SentOnce == [][last'.acts.send # None => (Listening /\ phase' = "idle")]_vars

\* Heard text that a failure cuts off goes to the draft.
NothingLost == [][(last'.op = "fail" /\ MicBusy /\ heard) => last'.acts.draft]_vars

\* A refusal leaves the turn denied, never listening.
DeniedIsDenied == [][(last'.op = "permission" /\ phase = "asking" /\ permission' = "refused") => phase' = "denied"]_vars

\* Every listen ends.
ListenEnds == Listening ~> phase = "idle"

=============================================================================
