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
(*   secrets   the server's features line before the first send named      *)
(*             "secrets" (docs/secrets.md)                                 *)
(*   asking    the ask number of the open ask-secret the running turn      *)
(*             waits on, 0 for none                                        *)
(*   asks      the last ask number this driver minted                      *)
(*                                                                         *)
(* History, for the properties: sent (the turns sent), dones (the dones    *)
(* written per turn), wrote (what the last step wrote), want (the latest   *)
(* value of each setting received), turnWant (want at the open turn's     *)
(* send), act (whether the last step was a stop or secret, and which id).  *)
(*                                                                         *)
(* The server is the environment: its features before any send, sends      *)
(* (each after the last turn's done, sequence rule 3), stops for any turn  *)
(* at any time, settings, secret answers, and the end of input. So is the  *)
(* harness: the credential check's answer, the query reading a prompt,     *)
(* text, tools, nexus_ask_secret calls, an assistant's error, the result,  *)
(* a refusal (an API key, overage), the stream failing, a --resume id it   *)
(* does not know, and an interrupt's answer.                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC

CONSTANTS
    MaxTurn,    \* turns are 1..MaxTurn
    MaxAsk,     \* secret asks are 1..MaxAsk (0 in instances without them)
    Fields,     \* the settings (the code has model and effort)
    Values,     \* their values
    Resumes     \* how the driver may be started: "none", or "given" (--resume)

None == "none"
Start == "start"
Turns == 1..MaxTurn
Asks == 1..MaxAsk
Ids == (0..MaxTurn) \cup (0..MaxAsk)
Stages == {"none", "waiting", "applying", "pushed", "taken"}
Failures == {None, "harness", "no-credential", "rate-limit", "usage-limit", "model-limit"}
Windows == {None, "harness", "usage-limit", "model-limit"}
NoChanges == [f \in Fields |-> None]
Changes == [Fields -> Values \cup {None}]
Settled == [Fields -> Values \cup {Start}]
Kinds == {"text", "text-end", "tool", "ask-secret", "done-ok", "done-stopped", "done-error",
          "error-harness", "error-no-credential", "error-session-not-found", "error-usage-limit", "error-model-limit"}

VARIABLES phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
          secrets, asking, asks, sent, dones, wrote, want, turnWant, act

secretVars == <<secrets, asking, asks>>
driver == <<phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
            secrets, asking, asks>>
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
    /\ secrets \in BOOLEAN
    /\ asks \in 0..MaxAsk
    /\ asking \in 0..asks
    /\ sent \subseteq Turns
    /\ dones \in [Turns -> Nat]
    /\ wrote \in Seq([k : Kinds, t : Ids])
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
    /\ secrets = FALSE
    /\ asking = 0
    /\ asks = 0
    /\ sent = {}
    /\ dones = [n \in Turns |-> 0]
    /\ wrote = <<>>
    /\ want = [f \in Fields |-> Start]
    /\ turnWant = [f \in Fields |-> Start]
    /\ act = [a |-> "init", n |-> 0]

Init == \E r \in Resumes : InitWith(r)

\* The open turn ends with its done (finish): its unapplied settings go back to wait for the next send, under any
\* newer ones, and any open secret ask is dropped. `before` is what is written ahead of the done.
FinishWith(outcome, before) ==
    /\ wrote' = before \o <<W("done-" \o outcome, open)>>
    /\ dones' = [dones EXCEPT ![open] = @ + 1]
    /\ open' = 0
    /\ stage' = "none"
    /\ stopping' = FALSE
    /\ textOpen' = FALSE
    /\ failure' = None
    /\ asking' = 0
    /\ carry' = NoChanges
    /\ pending' = Over(carry, pending)
    /\ UNCHANGED <<phase, resume, intFor, inUse, last, secrets, asks, sent, want, turnWant>>

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
    /\ asking' = 0
    /\ carry' = NoChanges
    /\ UNCHANGED <<resume, intFor, pending, inUse, last, secrets, asks, sent, want, turnWant>>

Refuse == RefuseWith(CloseText)

\* Claude does not know the --resume id (sequence rule 10): the old harness is closed (with any interrupt unanswered
\* and any open ask cancelled), a new session starts with the settings in use, and the open turn's prompt goes into it
\* once its credential check passes. A turn being stopped ends there: nothing runs any more.
NotFoundStopping(settings) ==
    /\ phase # "ended"
    /\ resume = "given"
    /\ open # 0 /\ stopping
    /\ resume' = "none"
    /\ phase' = "checking"
    /\ intFor' = 0
    /\ asking' = 0
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
    /\ UNCHANGED <<last, secrets, asks, sent, want, turnWant>>

\* (A witness takes NotFoundOn alone: a turn being stopped waits again, with nothing left to stop.)
NotFoundOn(settings) ==
    /\ phase # "ended"
    /\ resume = "given"
    /\ resume' = "none"
    /\ phase' = "checking"
    /\ intFor' = 0
    /\ asking' = 0
    /\ inUse' = settings
    /\ wrote' = CloseText \o <<W("error-session-not-found", open)>>
    /\ stage' = IF stage \in {"applying", "pushed", "taken"} THEN "waiting" ELSE stage
    /\ textOpen' = FALSE
    /\ failure' = None
    /\ UNCHANGED <<open, stopping, pending, carry, last, secrets, asks, sent, dones, want, turnWant>>

NotFoundGoingOn(settings) == ~(open # 0 /\ stopping) /\ NotFoundOn(settings)

NotFound == NotFoundStopping(inUse) \/ NotFoundGoingOn(inUse)

\* The driver exits on a failure it cannot go on from: the cause with the open turn, which the server cuts.
Fail ==
    /\ phase # "ended"
    /\ phase' = "ended"
    /\ asking' = 0
    /\ wrote' = <<W("error-harness", open)>>
    /\ UNCHANGED <<resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secrets, asks, ghost>>

-----------------------------------------------------------------------------
\* The server.

\* The server's features line, written right after hello and before any send (docs/driver-protocol.md).
ServerSecrets ==
    /\ MaxAsk > 0
    /\ phase # "ended"
    /\ ~secrets
    /\ last = 0
    /\ open = 0
    /\ secrets' = TRUE
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   asking, asks, ghost>>

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
    /\ UNCHANGED <<phase, resume, stopping, textOpen, failure, intFor, inUse, secretVars, dones, want>>

\* A stop ends what runs when it arrives: a prompt not yet read by the query is withdrawn and the turn ends at once;
\* one the query runs is interrupted, and any open secret ask is cancelled so the tool call settles without waiting on
\* the server's dialog. A stop for a turn that is not open, or already stopping, changes nothing.
StopRunning(n) ==
    /\ stopping' = TRUE
    /\ intFor' = n
    /\ asking' = 0
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, textOpen, failure, pending, carry, inUse, last, secrets, asks, ghost>>

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
                   secretVars, sent, dones, turnWant>>

\* The server's answer to ask `a`: settles the open ask when `a` is it, and is skipped otherwise (docs/secrets.md).
SecretWith(a, match) ==
    /\ phase # "ended"
    /\ a \in Asks
    /\ act' = [a |-> "secret", n |-> a]
    /\ asking' = IF match THEN 0 ELSE asking
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secrets, asks, ghost>>

Secret(a) == SecretWith(a, a = asking)

EndInput ==
    /\ phase # "ended"
    /\ phase' = "ended"
    /\ asking' = 0
    /\ wrote' = <<>>
    /\ UNCHANGED <<resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secrets, asks, ghost>>

-----------------------------------------------------------------------------
\* The driver's own steps.

CheckPass ==
    /\ phase = "checking"
    /\ phase' = "ready"
    /\ wrote' = <<>>
    /\ UNCHANGED <<resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secretVars, ghost>>

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
    /\ UNCHANGED <<phase, resume, open, stopping, textOpen, failure, intFor, pending, last, secretVars, ghost>>

Push ==
    /\ phase = "ready"
    /\ stage = "applying"
    /\ stage' = "pushed"
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secretVars, ghost>>

-----------------------------------------------------------------------------
\* The harness.

Take ==
    /\ phase # "ended"
    /\ stage = "pushed"
    /\ stage' = "taken"
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secretVars, ghost>>

Answer ==
    /\ phase # "ended"
    /\ intFor # 0
    /\ intFor' = 0
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, failure, pending, carry, inUse, last,
                   secretVars, ghost>>

\* What the query writes is the open turn's while its prompt is in the query; otherwise it is dropped.
Running == phase = "ready" /\ stage \in {"pushed", "taken"}

Unchanged == UNCHANGED <<phase, resume, open, stage, stopping, failure, intFor, pending, carry, inUse, last,
                         secretVars, ghost>>

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

\* The AI calls nexus_ask_secret(account, what, why) (docs/secrets.md): while a turn runs without stopping, after a
\* server line naming secrets, with no other ask open, the driver mints the next ask number, writes ask-secret and
\* waits for its secret line; otherwise it answers the tool locally (no-client, busy or cancelled) without writing.
AskSent ==
    /\ Running
    /\ ~stopping
    /\ secrets
    /\ asking = 0
    /\ asks < MaxAsk
    /\ asks' = asks + 1
    /\ asking' = asks + 1
    /\ wrote' = <<W("ask-secret", asks + 1)>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secrets, ghost>>

AskRefused ==
    /\ MaxAsk > 0
    /\ Running
    /\ (~secrets \/ asking # 0 \/ stopping)
    /\ wrote' = <<>>
    /\ UNCHANGED <<driver, ghost>>

Ask == AskSent \/ AskRefused

AssistantError(code) ==
    /\ Running
    /\ failure' = code
    /\ wrote' = <<>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, intFor, pending, carry, inUse, last,
                   secretVars, ghost>>

\* The query's result: "ok", "error", or "lost" (an error saying Claude does not know the session). While an ask is
\* open in the driver, the query waits on the tool call and emits no result until the ask settles (by its secret line
\* or a stop).
\* window is the latest rejected subscription window observed by the time the SDK result arrives.
ResultError(window) ==
    IF failure = "model-limit" THEN "error-model-limit"
    ELSE IF window # None /\ failure \in {None, "rate-limit", "usage-limit"}
         THEN CASE window = "model-limit" -> "error-model-limit"
                [] window = "usage-limit" -> "error-usage-limit"
                [] OTHER -> "error-harness"
    ELSE IF failure = "usage-limit" THEN "error-usage-limit" ELSE "error-harness"

ResultWindow(kind, window, refuse, lost) ==
    /\ Running
    /\ asking = 0
    /\ IF stopping THEN Finish("stopped")
       ELSE IF kind = "ok" /\ failure = None THEN Finish("ok")
       ELSE IF failure = "no-credential" THEN refuse
       ELSE IF failure = None /\ kind = "lost" /\ resume = "given" THEN lost
       ELSE FinishWith("error", CloseText \o <<W(ResultError(window), open)>>)

ResultWith(kind, refuse, lost) == \E window \in Windows : ResultWindow(kind, window, refuse, lost)

Result(kind) == ResultWith(kind, Refuse, NotFound)

ResultKinds == {"ok", "error", "lost"}

\* After the end, the driver does nothing more.
Ended == phase = "ended" /\ UNCHANGED vars

-----------------------------------------------------------------------------

\* Every step but a stop or a secret answer leaves `act` saying only that it was neither.
S(A) == A /\ act' = [a |-> "step", n |-> 0]

\* A witness passes its broken stop, apply, result, refusal, lost session, ask or secret in place of the real one.
NextSecretsWith(StopOp(_), ApplyOp, ResultOp(_), RefuseOp, NotFoundOp, AskOp, SecretOp(_)) ==
    \/ S(ServerSecrets)
    \/ \E n \in Turns : S(Send(n))
    \/ \E n \in Turns : StopOp(n)
    \/ \E c \in Changes : S(Settings(c))
    \/ \E a \in Asks : SecretOp(a)
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
    \/ S(AskOp)
    \/ \E code \in Failures \ {None} : S(AssistantError(code))
    \/ \E k \in ResultKinds : S(ResultOp(k))
    \/ S(RefuseOp)
    \/ S(Fail)
    \/ S(NotFoundOp)
    \/ Ended

NextWith(StopOp(_), ApplyOp, ResultOp(_), RefuseOp, NotFoundOp) ==
    NextSecretsWith(StopOp, ApplyOp, ResultOp, RefuseOp, NotFoundOp, Ask, Secret)

Next == NextWith(Stop, Apply, Result, Refuse, NotFound)

\* The driver's own steps, the harness's answers, and the server's bounded reply to an open secret ask (Secrets.tla
\* EveryRequestEnds: answered at once or by its deadline) are taken up; otherwise the server need do nothing. The
\* credential check answers (but a refusal or failure later is never forced). (TLC takes no operator arguments in a
\* temporal formula, so a liveness witness writes its own.)
Spec ==
    /\ Init /\ [][Next]_vars
    /\ WF_vars(S(phase = "checking" /\ (CheckPass \/ Refuse \/ Fail \/ NotFound)))
    /\ WF_vars(S(Apply)) /\ WF_vars(S(Push)) /\ WF_vars(S(Take)) /\ WF_vars(S(Answer))
    /\ WF_vars(\E a \in Asks : asking = a /\ Secret(a))
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

\* An ask is open in the driver only while a turn runs without stopping, on a server that offered secrets.
AskOnlyRunning == asking # 0 => Running /\ ~stopping /\ secrets

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

HasAsk(s) == \E i \in 1..Len(s) : s[i].k = "ask-secret"

\* An ask-secret is written only while a turn runs without stopping, after a server line naming secrets, with no other
\* ask open, and with the next ask number; secrets is enabled only before the first send.
AskGate ==
    [][/\ HasAsk(wrote') => secrets /\ Running /\ ~stopping /\ asking = 0 /\ asks' = asks + 1 /\ asking' = asks'
       /\ ~secrets /\ secrets' => last = 0 /\ open = 0]_vars

\* Only the open ask's own secret answer settles it; an answer for any other ask (its turn stopped) changes nothing.
SecretMatchesAsk ==
    [][/\ act'.a = "secret" /\ act'.n # asking => UNCHANGED driver /\ wrote' = <<>>
       /\ act'.a = "secret" /\ act'.n = asking => asking' = 0 /\ wrote' = <<>>]_vars

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
