----------------------------- MODULE VoiceTurn -----------------------------
(***************************************************************************)
(* The watch's voice turn, nexus::watch::VoiceTurn (watch/core/voice_turn.h). *)
(* The person holds to talk (Listening); the recording goes to the phone's   *)
(* recognizer (Transcribing); its words come back and the AI is chosen  *)
(* (Choosing); the prompt waits in the outbox until the server has it       *)
(* (Sending), then for the AI's reply (Waiting), which is read aloud    *)
(* (Speaking) unless the person stops it (Interrupted). Without a server    *)
(* through the phone the turn is Disconnected.                              *)
(*                                                                         *)
(* The owner's check: `unlocked` is the watch unlocked and on the wrist     *)
(* since its passcode. Locked, the turn neither listens nor reads a reply,  *)
(* and says so; a reply waits, unread, until Unlock.                        *)
(*                                                                         *)
(* Text is abstract: what the matcher made of the words is the Choose     *)
(* action's argument (the route), and the matcher itself is unit tested.   *)
(* Time is abstract: Tick(due) is the clock passing the 15 s the words get. *)
(* A reply is a count: which words it holds does not decide anything here.  *)
(*                                                                         *)
(* `last` is a history variable, the event just taken, its arguments and   *)
(* what it returned; the action properties are stated through it.          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Ais,        \* the server's AIs
    MaxPrompts,     \* prompts made, at most
    MaxUtter,       \* recordings, at most
    MaxReplies,     \* replies queued, at most
    MaxSaid         \* notices and cues said, at most (a bound for the model check only)

None == ""
Phases == {"idle", "listening", "transcribing", "choosing", "sending", "waiting", "speaking", "interrupted",
           "disconnected"}
\* named: an AI's name and words; none: words with no name, for the first AI in the server's list (f); nameonly: a
\* name with nothing to tell it; noais: words before the server has sent its AIs.
Routes == {"named", "nameonly", "ambiguous", "none", "empty", "noais"}
Talking == {"listening", "transcribing", "choosing"}
Resting == {"idle", "waiting", "sending"}

VARIABLES phase, connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues, last,
          unlocked

vars == <<phase, connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues, last,
          unlocked>>
state == <<phase, connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, unlocked>>

\* Where a turn rests once nothing is under way, and the rule that a reply is read as soon as it can be.
Rest(c, u, a) == IF ~c THEN "disconnected" ELSE IF u # {} THEN "sending" ELSE IF a # 0 THEN "waiting" ELSE "idle"
\* (Only unlocked: a reply is never read, or shown, while the watch is locked.)
Settle(p, r) == IF p \in Resting /\ r > 0 /\ unlocked THEN "speaking" ELSE p

Min(S) == CHOOSE m \in S : \A n \in S : m <= n

Did(act, ok) == last' = [act |-> act, ok |-> ok]
Cue == cues' = cues + 1
Notice == notices' = notices + 1
Refused(act) == Did(act, FALSE) /\ UNCHANGED state /\ UNCHANGED <<notices, cues>>
RefusedSaid(act) == Did(act, FALSE) /\ UNCHANGED state /\ Notice /\ UNCHANGED cues

Init ==
    /\ phase = "disconnected" /\ connected = FALSE /\ current = None
    /\ utterance = 0 /\ heard = 0 /\ unapplied = {} /\ nextPrompt = 1 /\ awaiting = 0 /\ replies = 0
    /\ notices = 0 /\ cues = 0 /\ last = [act |-> "init", ok |-> TRUE] /\ unlocked = FALSE

Press(mic) ==
    \/ phase = "disconnected" /\ RefusedSaid("press")
    \/ phase = "speaking" /\ phase' = "interrupted" /\ Did("press", TRUE)
       /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices>>
    \/ phase \in {"idle", "waiting", "interrupted"} /\
       IF ~unlocked THEN RefusedSaid("press")
       ELSE IF mic THEN phase' = "listening" /\ Did("press", TRUE)
                   /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices>>
              ELSE RefusedSaid("press")
    \/ phase \in {"listening", "transcribing", "choosing", "sending"} /\ Refused("press")

Release(long) ==
    IF phase # "listening" THEN Refused("release")
    ELSE IF long /\ utterance < MaxUtter
         THEN /\ utterance' = utterance + 1 /\ heard' = utterance + 1 /\ phase' = "transcribing"
              /\ Did("release", TRUE)
              /\ UNCHANGED <<connected, current, unapplied, nextPrompt, awaiting, replies, notices>>
         ELSE /\ ~long
              /\ phase' = Settle(Rest(connected, unapplied, awaiting), replies) /\ Notice /\ Did("release", TRUE)
              /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies>>

Transcript(u) ==
    IF phase = "transcribing" /\ u = heard
    THEN phase' = "choosing" /\ Did("transcript", TRUE)
         /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices>>
    ELSE Refused("transcript")

TranscriptFailed(u) ==
    IF phase = "transcribing" /\ u = heard
    THEN phase' = Settle(Rest(connected, unapplied, awaiting), replies) /\ Notice /\ Did("failed", TRUE)
         /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies>>
    ELSE Refused("failed")

\* A prompt to AI f: the next number, kept until the server has it.
SendTo(f) ==
    /\ nextPrompt <= MaxPrompts
    /\ unapplied' = unapplied \cup {nextPrompt} /\ nextPrompt' = nextPrompt + 1 /\ current' = f
    /\ phase' = Settle(Rest(connected, unapplied', awaiting), replies)
    /\ Cue /\ UNCHANGED notices

Choose(route, f) ==
    IF phase # "choosing" THEN Refused("choose")
    ELSE /\ last' = [act |-> "choose", ok |-> TRUE]
         /\ UNCHANGED <<connected, utterance, heard, awaiting, replies>>
         /\ CASE route \in {"named", "none"} -> SendTo(f)
              [] OTHER -> phase' = Settle(Rest(connected, unapplied, awaiting), replies) /\ Notice
                          /\ UNCHANGED <<current, unapplied, nextPrompt, cues>>

Applied(upTo) ==
    LET done == {n \in unapplied : n <= upTo}
        p1 == IF phase = "sending" THEN Rest(connected, unapplied \ done, awaiting + Cardinality(done)) ELSE phase
    IN IF done = {} \/ \E n \in unapplied \ done : n < Min(done) THEN Refused("applied")
       ELSE /\ unapplied' = unapplied \ done /\ awaiting' = awaiting + Cardinality(done)
            /\ phase' = Settle(p1, replies) /\ Did("applied", TRUE)
            /\ UNCHANGED <<connected, current, utterance, heard, nextPrompt, replies, notices>>

LostPrompt(n) ==
    IF n \notin unapplied THEN Refused("lostprompt")
    ELSE LET p1 == IF phase = "sending" THEN Rest(connected, unapplied \ {n}, awaiting) ELSE phase
         IN /\ unapplied' = unapplied \ {n} /\ phase' = Settle(p1, replies) /\ Notice /\ Did("lostprompt", TRUE)
            /\ UNCHANGED <<connected, current, utterance, heard, nextPrompt, awaiting, replies>>

Reply ==
    /\ replies < MaxReplies
    /\ replies' = replies + 1
    /\ awaiting' = IF awaiting > 0 THEN awaiting - 1 ELSE 0
    /\ LET p1 == IF phase = "waiting" THEN Rest(connected, unapplied, awaiting') ELSE phase
       IN phase' = Settle(p1, replies')
    /\ Did("reply", TRUE)
    /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, notices>>

SpeechDone ==
    IF phase # "speaking" THEN Refused("spoken")
    ELSE /\ replies' = replies - 1 /\ phase' = Settle(Rest(connected, unapplied, awaiting), replies - 1)
         /\ Did("spoken", TRUE)
         /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, notices>>

Resume ==
    IF phase # "interrupted" THEN Refused("resume")
    ELSE /\ phase' = Settle(Rest(connected, unapplied, awaiting), replies) /\ Did("resume", TRUE)
         /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices>>

Dismiss ==
    IF phase # "interrupted" THEN Refused("dismiss")
    ELSE /\ replies' = 0 /\ phase' = Rest(connected, unapplied, awaiting) /\ Did("dismiss", TRUE)
         /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, notices>>

Lost ==
    /\ connected' = FALSE
    /\ IF phase = "disconnected" THEN UNCHANGED <<phase, notices>> ELSE phase' = "disconnected" /\ Notice
    /\ Did("lost", TRUE)
    /\ UNCHANGED <<current, utterance, heard, unapplied, nextPrompt, awaiting, replies>>

Back ==
    /\ connected' = TRUE
    /\ IF phase = "disconnected"
       THEN phase' = Settle(Rest(TRUE, unapplied, awaiting), replies) /\ Did("back", TRUE) /\ Cue
       ELSE UNCHANGED <<phase, cues>> /\ Did("back", FALSE)
    /\ UNCHANGED <<current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices>>

Tick(due) ==
    IF due /\ phase = "transcribing"
    THEN phase' = Settle(Rest(connected, unapplied, awaiting), replies) /\ Notice /\ Did("tick", TRUE)
         /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies>>
    ELSE Refused("tick")

\* The watch locks (taken off the wrist, or locked): talking stops and a reply being read stops, each said; replies
\* are kept, unread.
Lock ==
    /\ unlocked' = FALSE
    /\ IF phase \in {"listening", "speaking", "interrupted"}
       THEN phase' = Rest(connected, unapplied, awaiting) /\ Notice
       ELSE UNCHANGED <<phase, notices>>
    /\ Did("lock", TRUE)
    /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, cues>>

\* The owner's check passed: what waits is read.
Unlock ==
    /\ unlocked' = TRUE
    /\ phase' = IF phase \in Resting /\ replies > 0 THEN "speaking" ELSE phase
    /\ Did("unlock", TRUE)
    /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues>>

\* Every action but Choose and Back says no cue.
Quiet ==
    \/ \E mic \in BOOLEAN : Press(mic)
    \/ \E long \in BOOLEAN : Release(long)
    \/ \E u \in 0..MaxUtter + 1 : Transcript(u) \/ TranscriptFailed(u)
    \/ \E n \in 0..MaxPrompts + 1 : Applied(n) \/ LostPrompt(n)
    \/ Reply \/ SpeechDone \/ Resume \/ Dismiss \/ Lost
    \/ \E due \in BOOLEAN : Tick(due)

Next ==
    \/ Quiet /\ UNCHANGED <<cues, unlocked>>
    \/ (\E r \in Routes, f \in Ais : Choose(r, f)) /\ UNCHANGED unlocked
    \/ Back /\ UNCHANGED unlocked
    \/ Lock \/ Unlock

Spec == Init /\ [][Next]_vars
\* The clock always reaches the words' deadline, and a reply being read is always read to its end.
\* What is said only counts: a model check bounds how many times, here and in the instances' CONSTRAINT.
Bounded == notices <= MaxSaid /\ cues <= MaxSaid

FairSpec == Spec /\ WF_vars(Tick(TRUE) /\ UNCHANGED <<cues, unlocked>>)
                 /\ WF_vars(SpeechDone /\ UNCHANGED <<cues, unlocked>>)

-----------------------------------------------------------------------------
\* Properties.

TypeOK ==
    /\ phase \in Phases /\ connected \in BOOLEAN /\ current \in Ais \cup {None}
    /\ utterance \in 0..MaxUtter /\ heard \in 0..utterance
    /\ unapplied \subseteq 1..MaxPrompts /\ nextPrompt \in 1..MaxPrompts + 1
    /\ awaiting \in Nat /\ replies \in 0..MaxReplies /\ notices \in Nat /\ cues \in Nat /\ unlocked \in BOOLEAN

\* The microphone records only with a server to send to; a turn without one says so.
TalkOnlyConnected == phase \in Talking => connected
DisconnectedExactly == (phase = "disconnected") <=> ~connected
\* Sending is exactly a prompt not yet applied, and every one has a number already handed out.
SendingExactly == phase \in Resting => ((phase = "sending") <=> unapplied # {})
PromptsNumbered == \A n \in unapplied : n < nextPrompt
\* Something is read only when there is something to read, and a reply waiting is read as soon as nothing is under way.
SpeakingHasReply == phase \in {"speaking", "interrupted"} => replies > 0
NoReplyLeftUnread == phase \in Resting /\ unlocked => replies = 0
\* Locked, the watch neither listens nor reads (or shows) a reply.
LockedQuiet == ~unlocked => phase \notin {"listening", "speaking", "interrupted"}

\* Nothing fails silently: a press refused, a lost server, a turn that ends with nothing sent, each says why.
PressRefusedSays == [][last'.act = "press" /\ ~last'.ok /\ phase \notin {"listening", "transcribing", "choosing", "sending"}
                       => notices' = notices + 1]_vars
LostSays == [][last'.act = "lost" /\ phase # "disconnected" => notices' = notices + 1]_vars
TurnEndsSaid == [][(phase \in Talking /\ phase' \notin Talking /\ nextPrompt' = nextPrompt /\ phase' # "disconnected")
                   => notices' + cues' > notices + cues]_vars
\* Every prompt made is confirmed aloud: who it went to.
PromptConfirmed == [][nextPrompt' # nextPrompt => cues' = cues + 1]_vars
\* Only Choose makes a prompt, one at a time, numbered in order; a prompt leaves only applied or said lost.
PromptsOnlyChosen == [][nextPrompt' # nextPrompt => last'.act = "choose" /\ nextPrompt' = nextPrompt + 1]_vars
PromptsNeverDropped == [][\A n \in unapplied \ unapplied' :
                            last'.act = "applied" \/ (last'.act = "lostprompt" /\ notices' = notices + 1)]_vars
RefusedChangesNothing == [][~last'.ok => UNCHANGED state]_vars
\* Words are always awaited for a bounded time.
TranscribingEnds == phase = "transcribing" ~> phase # "transcribing"
=============================================================================
