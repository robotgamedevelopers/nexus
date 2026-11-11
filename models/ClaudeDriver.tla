---------------------------- MODULE ClaudeDriver ----------------------------
(***************************************************************************)
(* The Claude driver's turns: drivers/claude/src/driver.ts, the driver     *)
(* protocol (docs/driver-protocol.md) on one side and one Agent SDK query  *)
(* on the other.                                                            *)
(*                                                                         *)
(*   phase     the harness's credential check: checking, ready, or ended   *)
(*             (the driver exits; nothing more is written)                 *)
(*   resume    the harness was started with a --resume id ("given"), which *)
(*             Claude may not know; a new session starts with "none"      *)
(*   open      the open turn, 0 for none                                   *)
(*   stage     the open turn's prompt: waiting (behind the credential check*)
(*             or an unanswered interrupt), applying (its settings going   *)
(*             to the harness), pushed (in the query's input, still        *)
(*             withdrawable), taken (read by the query: it runs)           *)
(*   stopping  a stop was taken for the open turn                          *)
(*   textOpen  a text block of the open turn is open on the wire           *)
(*   failure   the error an assistant message reported for the open turn   *)
(*   intFor    the turn whose interrupt has not answered yet, 0 for none   *)
(*   pending   settings received since the last send, for the next one     *)
(*   carry     the open turn's settings, not yet applied                   *)
(*   inUse     the settings the harness runs with ("start": the driver's   *)
(*             own arguments)                                              *)
(*   last      the last turn sent                                          *)
(*                                                                         *)
(* History, for the properties: sent (the turns sent), dones (the dones    *)
(* written per turn), wrote (what the last step wrote), want (the latest   *)
(* value of each setting received), turnWant (want at the open turn's     *)
(* send), act (whether the last step was a stop, and of which turn).       *)
(*                                                                         *)
(* The server is the environment: sends (each after the last turn's done,  *)
(* sequence rule 3), stops for any turn at any time, settings, and the end *)
(* of input. So is the harness: the credential check's answer, the query   *)
(* reading a prompt, text, tools, an assistant's error, the result, a      *)
(* refusal (an API key, overage), the stream failing, a --resume id it     *)
(* does not know, and an interrupt's answer.                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC

CONSTANTS
    MaxTurn,    \* turns are 1..MaxTurn
    Fields,     \* the settings (the code has model and effort)
    Values,     \* their values
    Resumes     \* how the driver may be started: "none", or "given" (--resume)

None == "none"
Start == "start"
Turns == 1..MaxTurn
Stages == {"none", "waiting", "applying", "pushed", "taken"}
Failures == {None, "harness", "no-credential"}
NoChanges == [f \in Fields |-> None]
Changes == [Fields -> Values \cup {None}]
Settled == [Fields -> Values \cup {Start}]
Kinds == {"text", "text-end", "tool", "done-ok", "done-stopped", "done-error",
          "error-harness", "error-no-credential", "error-session-not-found"}

VARIABLES phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
          sent, dones, wrote, want, turnWant, act

driver == <<phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last>>
ghost == <<sent, dones, want, turnWant>>
vars == <<driver, ghost, wrote, act>>

TypeOK ==
    /\ phase \in {"checking", "ready", "ended"}
    /\ resume \in {"none", "given"}
    /\ open \in 0..MaxTurn
    /\ stage \in Stages
    /\ (open = 0) <=> (stage = "none")
    /\ stopping \in BOOLEAN
    /\ textOpen \in BOOLEAN
    /\ failure \in Failures
    /\ intFor \in 0..MaxTurn
    /\ pending \in Changes
    /\ carry \in Changes
    /\ inUse \in Settled
    /\ last \in 0..MaxTurn
    /\ sent \subseteq Turns
    /\ dones \in [Turns -> Nat]
    /\ wrote \in Seq([k : Kinds, t : 0..MaxTurn])
    /\ want \in Settled
    /\ turnWant \in Settled

W(k, n) == [k |-> k, t |-> n]

\* Settings `c` laid over `base`: a field `c` leaves None stays as it was.
Over(base, c) == [f \in Fields |-> IF c[f] = None THEN base[f] ELSE c[f]]

\* A text block left open is ended first (closeText).
CloseText == IF textOpen THEN <<W("text-end", open)>> ELSE <<>>

InitWith(r) ==
    /\ phase = "checking"
    /\ resume = r
    /\ open = 0
    /\ stage = "none"
    /\ stopping = FALSE
    /\ textOpen = FALSE
    /\ failure = None
    /\ intFor = 0
    /\ pending = NoChanges
    /\ carry = NoChanges
    /\ inUse = [f \in Fields |-> Start]
    /\ last = 0
    /\ sent = {}
    /\ dones = [n \in Turns |-> 0]
    /\ wrote = <<>>
    /\ want = [f \in Fields |-> Start]
    /\ turnWant = [f \in Fields |-> Start]
    /\ act = [a |-> "init", n |-> 0]

Init == \E r \in Resumes : InitWith(r)

\* The open turn ends with its done (finish): its unapplied settings go back to wait for the next send, under any
\* newer ones. `before` is what is written ahead of the done.
FinishWith(outcome, before) ==
    /\ wrote' = before \o <<W("done-" \o outcome, open)>>
    /\ dones' = [dones EXCEPT ![open] = @ + 1]
    /\ open' = 0
    /\ stage' = "none"
    /\ stopping' = FALSE
    /\ textOpen' = FALSE
    /\ failure' = None
    /\ carry' = NoChanges
    /\ pending' = Over(carry, pending)
    /\ UNCHANGED <<phase, resume, intFor, inUse, last, sent, want, turnWant>>

Finish(outcome) == FinishWith(outcome, CloseText)

\* No usable credential (protocol rule 11): the open turn's text block ended, the error with the turn, its done, exit.
RefuseWith(close) ==
    /\ phase # "ended"
    /\ phase' = "ended"
    /\ wrote' = close \o <<W("error-no-credential", open)>> \o (IF open # 0 THEN <<W("done-error", open)>> ELSE <<>>)
    /\ dones' = IF open # 0 THEN [dones EXCEPT ![open] = @ + 1] ELSE dones
    /\ open' = 0
    /\ stage' = "none"
    /\ stopping' = FALSE
    /\ textOpen' = FALSE
    /\ failure' = None
    /\ carry' = NoChanges
    /\ UNCHANGED <<resume, intFor, pending, inUse, last, sent, want, turnWant>>

Refuse == RefuseWith(CloseText)

\* Claude does not know the --resume id (sequence rule 10): the old harness is closed (with any interrupt unanswered),
\* a new session starts with the settings in use, and the open turn's prompt goes into it once its credential check
\* passes. A turn being stopped ends there: nothing runs any more.
NotFoundStopping(settings) ==
    /\ phase # "ended"
    /\ resume = "given"
    /\ open # 0 /\ stopping
    /\ resume' = "none"
    /\ phase' = "checking"
    /\ intFor' = 0
    /\ inUse' = settings
    /\ wrote' = CloseText \o <<W("error-session-not-found", open), W("done-stopped", open)>>
    /\ dones' = [dones EXCEPT ![open] = @ + 1]
    /\ open' = 0
    /\ stage' = "none"
    /\ stopping' = FALSE
    /\ textOpen' = FALSE
    /\ failure' = None
    /\ carry' = NoChanges
    /\ pending' = Over(carry, pending)
    /\ UNCHANGED <<last, sent, want, turnWant>>

\* (A witness takes NotFoundOn alone: a turn being stopped waits again, with nothing left to stop.)
NotFoundOn(settings) ==
    /\ phase # "ended"
    /\ resume = "given"
    /\ resume' = "none"
    /\ phase' = "checking"
    /\ intFor' = 0
    /\ inUse' = settings
    /\ wrote' = CloseText \o <<W("error-session-not-found", open)>>
    /\ stage' = IF stage \in {"applying", "pushed", "taken"} THEN "waiting" ELSE stage
    /\ textOpen' = FALSE
    /\ failure' = None
    /\ UNCHANGED <<open, stopping, pending, carry, last, sent, dones, want, turnWant>>

NotFoundGoingOn(settings) == ~(open # 0 /\ stopping) /\ NotFoundOn(settings)

NotFound == NotFoundStopping(inUse) \/ NotFoundGoingOn(inUse)

\* The driver exits on a failure it cannot go on from: the cause with the open turn, which the server cuts.
Fail ==
    /\ phase # "ended"
    /\ phase' = "ended"
    /\ wrote' = <<W("error-harness", open)>>
    /\ UNCHANGED <<resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last, ghost>>

-----------------------------------------------------------------------------
\* The server.

Send(n) ==
    /\ phase # "ended"
    /\ open = 0
    /\ n > last
    /\ open' = n
    /\ last' = n
    /\ stage' = "waiting"
    /\ carry' = pending
    /\ pending' = NoChanges
    /\ sent' = sent \cup {n}
    /\ turnWant' = want
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, stopping, textOpen, failure, intFor, inUse, dones, want>>

\* A stop ends what runs when it arrives: a prompt not yet read by the query is withdrawn and the turn ends at once;
\* one the query runs is interrupted. A stop for a turn that is not open, or already stopping, changes nothing.
StopRunning(n) ==
    /\ stopping' = TRUE
    /\ intFor' = n
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, textOpen, failure, pending, carry, inUse, last, ghost>>

StopIgnored == UNCHANGED <<driver, ghost>> /\ wrote' = <<>>

\* A witness passes its broken early stop (the prompt withdrawn) or running stop (an interrupt) in place of the real one.
StopWith(n, early, running) ==
    /\ phase # "ended"
    /\ act' = [a |-> "stop", n |-> n]
    /\ IF n # open \/ stopping THEN StopIgnored
       ELSE IF stage # "taken" THEN early
       ELSE running

Stop(n) == StopWith(n, Finish("stopped"), StopRunning(n))

Settings(c) ==
    /\ phase # "ended"
    /\ c # NoChanges
    /\ pending' = Over(pending, c)
    /\ want' = Over(want, c)
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, failure, intFor, carry, inUse, last,
                   sent, dones, turnWant>>

EndInput ==
    /\ phase # "ended"
    /\ phase' = "ended"
    /\ wrote' = <<>>
    /\ UNCHANGED <<resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last, ghost>>

-----------------------------------------------------------------------------
\* The driver's own steps.

CheckPass ==
    /\ phase = "checking"
    /\ phase' = "ready"
    /\ wrote' = <<>>
    /\ UNCHANGED <<resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last, ghost>>

\* A turn's start, behind every operation before it: an interrupt not yet answered holds it.
Apply ==
    /\ phase = "ready"
    /\ stage = "waiting"
    /\ ~stopping
    /\ intFor = 0
    /\ stage' = "applying"
    /\ inUse' = Over(inUse, carry)
    /\ carry' = NoChanges
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stopping, textOpen, failure, intFor, pending, last, ghost>>

Push ==
    /\ phase = "ready"
    /\ stage = "applying"
    /\ stage' = "pushed"
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stopping, textOpen, failure, intFor, pending, carry, inUse, last, ghost>>

-----------------------------------------------------------------------------
\* The harness.

Take ==
    /\ phase # "ended"
    /\ stage = "pushed"
    /\ stage' = "taken"
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stopping, textOpen, failure, intFor, pending, carry, inUse, last, ghost>>

Answer ==
    /\ phase # "ended"
    /\ intFor # 0
    /\ intFor' = 0
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, failure, pending, carry, inUse, last, ghost>>

\* What the query writes is the open turn's while its prompt is in the query; otherwise it is dropped.
Running == phase = "ready" /\ stage \in {"pushed", "taken"}

Unchanged == UNCHANGED <<phase, resume, open, stage, stopping, failure, intFor, pending, carry, inUse, last, ghost>>

Delta ==
    /\ Running
    /\ textOpen' = TRUE
    /\ wrote' = <<W("text", open)>>
    /\ Unchanged

BlockEnd ==
    /\ Running
    /\ textOpen' = FALSE
    /\ wrote' = CloseText
    /\ Unchanged

\* Text that came only in an assistant message: a whole block.
BlockText ==
    /\ Running
    /\ textOpen' = FALSE
    /\ wrote' = <<W("text", open), W("text-end", open)>>
    /\ Unchanged

Tool ==
    /\ Running
    /\ textOpen' = FALSE
    /\ wrote' = CloseText \o <<W("tool", open)>>
    /\ Unchanged

AssistantError(code) ==
    /\ Running
    /\ failure' = code
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, intFor, pending, carry, inUse, last, ghost>>

\* The query's result: "ok", "error", or "lost" (an error saying Claude does not know the session).
ResultWith(kind, refuse, lost) ==
    /\ Running
    /\ IF stopping THEN Finish("stopped")
       ELSE IF kind = "ok" /\ failure = None THEN Finish("ok")
       ELSE IF failure = "no-credential" THEN refuse
       ELSE IF failure = None /\ kind = "lost" /\ resume = "given" THEN lost
       ELSE FinishWith("error", CloseText \o <<W("error-harness", open)>>)

Result(kind) == ResultWith(kind, Refuse, NotFound)

ResultKinds == {"ok", "error", "lost"}

\* After the end, the driver does nothing more.
Ended == phase = "ended" /\ UNCHANGED vars

-----------------------------------------------------------------------------

\* Every step but a stop leaves `act` saying only that it was not a stop.
S(A) == A /\ act' = [a |-> "step", n |-> 0]

\* A witness passes its broken stop, apply, result, refusal or lost session in place of the real one.
NextWith(StopOp(_), ApplyOp, ResultOp(_), RefuseOp, NotFoundOp) ==
    \/ \E n \in Turns : S(Send(n))
    \/ \E n \in Turns : StopOp(n)
    \/ \E c \in Changes : S(Settings(c))
    \/ S(EndInput)
    \/ S(CheckPass)
    \/ S(ApplyOp)
    \/ S(Push)
    \/ S(Take)
    \/ S(Answer)
    \/ S(Delta)
    \/ S(BlockEnd)
    \/ S(BlockText)
    \/ S(Tool)
    \/ \E code \in {"harness", "no-credential"} : S(AssistantError(code))
    \/ \E k \in ResultKinds : S(ResultOp(k))
    \/ S(RefuseOp)
    \/ S(Fail)
    \/ S(NotFoundOp)
    \/ Ended

Next == NextWith(Stop, Apply, Result, Refuse, NotFound)

\* The driver's own steps and the harness's answers are taken up; the server need do nothing. The credential check
\* answers (but a refusal or failure later is never forced). (TLC takes no operator arguments in a temporal formula,
\* so a liveness witness writes its own.)
Spec ==
    /\ Init /\ [][Next]_vars
    /\ WF_vars(S(phase = "checking" /\ (CheckPass \/ Refuse \/ Fail \/ NotFound)))
    /\ WF_vars(S(Apply)) /\ WF_vars(S(Push)) /\ WF_vars(S(Take)) /\ WF_vars(S(Answer))
    /\ WF_vars(\E k \in ResultKinds : S(Result(k)))

\* Without the fairness: what a finite code trace is checked against.
SafeSpec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Properties

\* Every send gets exactly one done: none while its turn is open (the driver may exit then, and the server cuts it),
\* one once it has closed.
OneDone == \A n \in Turns : dones[n] = IF n \in sent /\ n # open THEN 1 ELSE 0

\* An interrupt lands on the turn it was sent for: while it has not answered, no later turn's prompt is in the query.
StopNeverLater == intFor # 0 /\ stage \in {"pushed", "taken"} => open = intFor

\* What one step writes keeps the wire's text blocks (protocol rule 5): a text-end only for an open block, and no tool
\* or done while one is open.
RECURSIVE BlocksKept(_, _)
BlocksKept(o, s) ==
    IF s = <<>> THEN TRUE
    ELSE LET h == Head(s) IN
         CASE h.k = "text"                                -> BlocksKept(TRUE, Tail(s))
           [] h.k = "text-end"                            -> o /\ BlocksKept(FALSE, Tail(s))
           [] h.k \in {"tool", "done-ok", "done-stopped", "done-error"} -> ~o /\ BlocksKept(o, Tail(s))
           [] OTHER                                       -> BlocksKept(o, Tail(s))

NoDoneInBlock == [][BlocksKept(textOpen, wrote')]_vars

\* Settings never change a turn whose prompt is in the query.
SettingsNeverChangeOpen == [][open # 0 /\ open' = open /\ stage \in {"pushed", "taken"} => inUse' = inUse]_vars

\* A turn runs with every setting received before its send, and none after: a stop before it began carries them on.
SettingsFromNextSend == [][stage' = "pushed" /\ stage # "pushed" => inUse' = turnWant]_vars

\* A stop for the open turn ends what runs: the turn's done, or an interrupt of it.
StopEndsWhatRuns ==
    [][act'.a = "stop" /\ act'.n = open /\ open # 0 /\ ~stopping /\ phase # "ended" =>
         dones'[open] = dones[open] + 1 \/ (stopping' /\ intFor' = open)]_vars

\* A stop that finds nothing of its turn running changes nothing, and nothing but a stop for the open turn marks it
\* stopping: no stop is kept for a later turn.
NoLatchedStop ==
    [][/\ act'.a = "stop" /\ (act'.n # open \/ stopping) => UNCHANGED driver /\ wrote' = <<>>
       /\ ~stopping /\ stopping' => act'.a = "stop" /\ act'.n = open]_vars

\* Every turn sent ends with its done, unless the driver exits first.
EveryTurnEnds == \A n \in Turns : open = n ~> (open # n \/ phase = "ended")

\* An interrupt is answered or dropped with its harness, so the next turn is never held for good (unless the driver
\* exits, which ends everything).
InterruptsAnswered == intFor # 0 ~> (intFor = 0 \/ phase = "ended")

=============================================================================
