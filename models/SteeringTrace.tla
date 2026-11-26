---------------------------- MODULE SteeringTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real code is a      *)
(* behaviour of Steering.tla. tests/driver/steering_walk.cpp drives the    *)
(* server's supervisor (src/server/supervisor.h) and a driver's side of    *)
(* steering (src/driver/steering.h) over encoded protocol lines, playing  *)
(* the person, the harness's moments and the driver's exit, and writes     *)
(* SteeringTraceData.tla beside this module: the instance's bounds and     *)
(* Trace, one record per step, each the step's op and every variable of    *)
(* the model after it, read from the code (the supervisor's ids mapped to  *)
(* the person's).                                                          *)
(*                                                                         *)
(* Each step of this spec takes the action the record names; TraceMatches  *)
(* requires the spec's state to equal the code's, variable by variable.    *)
(* The model's safety properties are checked on the trace. A step the     *)
(* spec cannot take is a deadlock, which TLC reports. The op nosettle is   *)
(* the walk offering the driver a settle that it refused (something was   *)
(* pending): nothing changes, and a settle the code took when the model    *)
(* would not is recorded as settle, which the spec cannot take.            *)
(***************************************************************************)
EXTENDS Naturals, Sequences, TLC, SteeringTraceData

VARIABLES feats, kinds, s, d, s2d, d2s, hlog, l

M == INSTANCE Steering WITH MaxIds <- TraceMaxIds, MaxStarts <- TraceMaxStarts, MaxTools <- TraceMaxTools,
                            MaxRetries <- TraceMaxRetries, MaxWaiting <- TraceMaxWaiting,
                            FeatureSets <- {Trace[1].feats}, Fault <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

all == <<feats, kinds, s, d, s2d, d2s, hlog, l>>

Step(op) ==
    CASE op = "prompt"   -> M!Prompt
      [] op = "steer"    -> M!Steer("steer")
      [] op = "follow"   -> M!Steer("follow")
      [] op = "stop"     -> M!Stop
      [] op = "srecv"    -> M!Receive
      [] op = "seen"     -> M!Exited
      [] op = "drecv"    -> M!Read
      [] op = "tools"    -> M!Tools
      [] op = "boundary" -> M!Boundary
      [] op = "runend"   -> M!RunEnd
      [] op = "retry"    -> M!Retry
      [] op = "settle"   -> M!Settle
      [] op = "nosettle" -> UNCHANGED <<feats, kinds, s, d, s2d, d2s, hlog>>
      [] op = "finish"   -> M!Finish
      [] op = "exit"     -> M!Exit

TraceInit == M!Init /\ l = 1
TraceNext == /\ l < Len(Trace)
             /\ l' = l + 1
             /\ Step(Trace[l + 1].op)
TraceEnd == l = Len(Trace) /\ UNCHANGED all
TraceSpec == TraceInit /\ [][TraceNext \/ TraceEnd]_all

TraceMatches == LET r == Trace[l] IN
    /\ feats = r.feats /\ kinds = r.kinds /\ s = r.s /\ d = r.d
    /\ s2d = r.s2d /\ d2s = r.d2s /\ hlog = r.hlog

\* The model's properties, on the trace (a cfg names no instance's operator).
AtMostOnce == M!AtMostOnce
NeverLost == M!NeverLost
WaitingFits == M!WaitingFits
ViewMatches == M!ViewMatches
NamedOnly == M!NamedOnly
DriverKeptRules == M!DriverKeptRules
ServerKeptRules == M!ServerKeptRules
CutOnlyOnExit == M!CutOnlyOnExit
FollowAtRunEnd == M!FollowAtRunEnd
SettleEmpty == M!SettleEmpty
=============================================================================
