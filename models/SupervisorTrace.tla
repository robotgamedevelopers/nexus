--------------------------- MODULE SupervisorTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real driver           *)
(* supervisor (src/server/supervisor.h) is a behaviour of Supervisor.tla.  *)
(*                                                                         *)
(* tests/server/supervisor_walk.cpp drives the code with a seeded random   *)
(* walk, playing the driver, the person and the clock, and writes          *)
(* SupervisorTraceData.tla beside this module. It defines the instance     *)
(* (TracePrompts, TraceMaxWaiting, TraceMaxFailedStarts, TraceMaxRestarts, *)
(* TraceSessions) and Trace: one record per step. Trace[1] is the start    *)
(* (op "init"); each later record is the event the code was given (op,    *)
(* and for a message its kind, turn and value), and after it the code's    *)
(* whole state, every send and stop it has written, the entries the step appended *)
(* to the tab, and the --resume of its last start.                         *)
(*                                                                         *)
(* Each step of this spec takes the Supervisor action the record names.    *)
(* TraceMatches requires the spec's state to equal the code's, field by   *)
(* field (all but the ghost fate). A step the spec cannot take is a        *)
(* deadlock. The model's safety properties are checked on the trace.       *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, SupervisorTraceData

VARIABLES s, sent, restarts, l

L == INSTANCE Supervisor WITH Prompts <- TracePrompts, MaxWaiting <- TraceMaxWaiting,
                              MaxFailedStarts <- TraceMaxFailedStarts, MaxRestarts <- TraceMaxRestarts,
                              Sessions <- TraceSessions

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Fields == {"phase", "stage", "open", "prompt", "next", "waiting", "queue", "session", "gathered", "finished",
           "failed", "blocked", "nocred", "down", "procs", "written", "stops", "fx", "resumed"}

Matches(k) ==
    LET r == Trace[k] IN
    /\ sent = r.sent
    /\ restarts = r.restarts
    /\ \A f \in Fields : s[f] = r[f]

TraceMatches == Matches(l)

Msg(r) == [kind |-> r.kind, t |-> r.t, v |-> r.v]

Step(r) ==
    CASE r.op = "prompt"   -> L!Prompt
      [] r.op = "queue"    -> L!QueuePrompt
      [] r.op = "stop"     -> L!Stop
      [] r.op = "message"  -> L!Message(Msg(r))
      [] r.op = "broken"   -> L!Broken
      [] r.op = "exit"     -> L!Exit
      [] r.op = "timeout"  -> L!Timeout
      [] r.op = "shutdown" -> L!Shutdown
      [] r.op = "restart"  -> L!Restart
      [] OTHER             -> FALSE

traceVars == <<s, sent, restarts, l>>

TraceInit == l = 1 /\ L!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* Supervisor's spec and properties, checked on the trace.
SupervisorSpec == L!SafetySpec
TypeOK == L!TypeOK
OneDriver == L!OneDriver
OneTurn == L!OneTurn
TurnsIncrease == L!TurnsIncrease
AtMostOnce == L!AtMostOnce
WaitingSound == L!WaitingSound
QueueSound == L!QueueSound
StopOnce == L!StopOnce
LimitHolds == L!LimitHolds
EffectsForOpen == L!EffectsForOpen
EndsAnsweredOrCut == L!EndsAnsweredOrCut
TextKept == L!TextKept
StartsSound == L!StartsSound
NoSendWithoutCredential == L!NoSendWithoutCredential
StopForOpen == L!StopForOpen
TakesAtTurnEnd == L!TakesAtTurnEnd

=============================================================================
