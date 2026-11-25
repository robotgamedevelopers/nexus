------------------------- MODULE ClaudeDriverTrace -------------------------
(***************************************************************************)
(* Trace validation: TLC checks that traces of the real Claude driver      *)
(* (drivers/claude/src/driver.ts) are behaviours of ClaudeDriver.tla.      *)
(*                                                                         *)
(* The driver records each step the model names through Host.step, with    *)
(* its state after the step; the test host adds what the step wrote.      *)
(* drivers/claude/test/walk.ts (a seeded random walk over the scripted     *)
(* query) and the driver's unit tests (test/unit/driver.test.ts) write     *)
(* ClaudeDriverTraceData.tla beside this module. It defines the instance   *)
(* (TraceMaxTurn, TraceMaxAsk, TraceFields, TraceValues) and Traces: one   *)
(* trace per driver's life, each a sequence of records. A trace's first    *)
(* record is the start (op "init"); each later one names the step and what *)
(* it was given (turn, ask, changes, kind, code), then the driver's state  *)
(* after it and what it wrote.                                             *)
(*                                                                         *)
(* TLC picks each trace in turn (k) and takes, at each record, the action  *)
(* it names. TraceMatches requires the spec's state to equal the code's,   *)
(* field by field; a step the spec cannot take is a deadlock. The model's  *)
(* properties are checked on every trace.                                  *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, ClaudeDriverTraceData

VARIABLES phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
          secrets, asking, asks, sent, dones, wrote, want, turnWant, act, k, l

D == INSTANCE ClaudeDriver WITH MaxTurn <- TraceMaxTurn, MaxAsk <- TraceMaxAsk, Fields <- TraceFields,
                                Values <- TraceValues, Resumes <- {"none", "given"}

ASSUME Len(Traces) >= 1 /\ \A i \in 1..Len(Traces) : Len(Traces[i]) >= 1 /\ Traces[i][1].op = "init"

Matches(r) ==
    /\ phase = r.phase
    /\ resume = r.resume
    /\ open = r.open
    /\ stage = r.stage
    /\ stopping = r.stopping
    /\ textOpen = r.textOpen
    /\ failure = r.failure
    /\ intFor = r.interrupting
    /\ pending = r.pending
    /\ carry = r.carry
    /\ inUse = r.inUse
    /\ last = r.last
    /\ secrets = r.secrets
    /\ asking = r.asking
    /\ asks = r.asks
    /\ wrote = r.wrote

TraceMatches == Matches(Traces[k][l])

Step(r) ==
    CASE r.op = "server"          -> D!S(D!ServerSecrets)
      [] r.op = "send"            -> D!S(D!Send(r.turn))
      [] r.op = "stop"            -> D!Stop(r.turn)
      [] r.op = "settings"        -> D!S(D!Settings(r.changes))
      [] r.op = "secret"          -> D!Secret(r.ask)
      [] r.op = "end"             -> D!S(D!EndInput)
      [] r.op = "checked"         -> D!S(D!CheckPass)
      [] r.op = "apply"           -> D!S(D!Apply)
      [] r.op = "push"            -> D!S(D!Push)
      [] r.op = "take"            -> D!S(D!Take)
      [] r.op = "answered"        -> D!S(D!Answer)
      [] r.op = "delta"           -> D!S(D!Delta)
      [] r.op = "block-end"       -> D!S(D!BlockEnd)
      [] r.op = "block-text"      -> D!S(D!BlockText)
      [] r.op = "tool"            -> D!S(D!Tool)
      [] r.op = "ask"             -> D!S(D!AskSent)
      [] r.op = "ask-refused"     -> D!S(D!AskRefused)
      [] r.op = "assistant-error" -> D!S(D!AssistantError(r.code))
      [] r.op = "result"          -> D!S(D!ResultWindow(r.kind, r.rejectedLimit, D!Refuse, D!NotFound))
      [] r.op = "refused"         -> D!S(D!Refuse)
      [] r.op = "failed"          -> D!S(D!Fail)
      [] r.op = "lost"            -> D!S(D!NotFound)
      [] OTHER                    -> FALSE

traceVars == <<phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
               secrets, asking, asks, sent, dones, wrote, want, turnWant, act, k, l>>

TraceInit == k \in 1..Len(Traces) /\ l = 1 /\ D!InitWith(Traces[k][1].resume)

TraceNext ==
    /\ l < Len(Traces[k])
    /\ l' = l + 1
    /\ k' = k
    /\ Step(Traces[k][l + 1])

\* the end of a trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Traces[k]) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* ClaudeDriver's spec (without its fairness, which a finite trace cannot meet) and its properties, checked on traces
ClaudeDriverSpec == D!SafeSpec
TypeOK == D!TypeOK
OneDone == D!OneDone
StopNeverLater == D!StopNeverLater
AskOnlyRunning == D!AskOnlyRunning
NoDoneInBlock == D!NoDoneInBlock
AskGate == D!AskGate
SecretMatchesAsk == D!SecretMatchesAsk
SettingsNeverChangeOpen == D!SettingsNeverChangeOpen
SettingsFromNextSend == D!SettingsFromNextSend
StopEndsWhatRuns == D!StopEndsWhatRuns
NoLatchedStop == D!NoLatchedStop

=============================================================================
