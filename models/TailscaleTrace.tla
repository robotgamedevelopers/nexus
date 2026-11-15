---------------------------- MODULE TailscaleTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real lifecycle code  *)
(* (src/tailscale/lifecycle/lifecycle.go: Step, derive and Table, the      *)
(* code the shim's watch runs for every node) is a behaviour of            *)
(* Tailscale.tla, its environment included.                                *)
(*                                                                         *)
(* The walk (src/tailscale/lifecycle/walk) plays the model's host and      *)
(* world on TraceN nodes and TraceD state directories: each step is one of *)
(* Tailscale.tla's actions, taken where the walk's copy of the world       *)
(* allows it, giving the code the event that action produces (with the     *)
(* texts tsnet gives: refusals, certificate and network errors, sign-in    *)
(* URLs). It writes TailscaleTraceData.tla beside this module: Trace, one  *)
(* record per step, with the action and its arguments, and after it the    *)
(* code's facts, its state, its reason, whether it shows a URL, its down   *)
(* flag, and the table's holders.                                          *)
(*                                                                         *)
(* Each step takes the action the record names, so the event must be one   *)
(* the model's world produces in that state (an event out of the world's   *)
(* order has no step), and pins the next state to the record (a state the  *)
(* code reached that the action does not give has none either). TLC        *)
(* reports either as a deadlock. The invariants of Tailscale.tla are       *)
(* checked on the trace as well.                                           *)
(***************************************************************************)
EXTENDS Tailscale, Sequences, TailscaleTraceData

VARIABLE l

traceVars == <<vars, l>>

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

\* The next state of node n is the code's.
Pin(r) ==
    /\ node'[r.n] = r.f
    /\ Phase(node'[r.n]) = r.phase
    /\ Reason(node'[r.n]) = r.reason
    /\ Shown(node'[r.n]) = r.url
    /\ ControlDown(node'[r.n]) = r.down
    /\ holder' = r.holder

Do(r) ==
    CASE r.op = "HostStart"          -> HostStart(r.n, r.d, r.k, r.c)
      [] r.op = "refused"            -> ~node[r.n].started /\ holder[r.d] # NoNode /\ UNCHANGED vars
      [] r.op = "HostClose"          -> HostClose(r.n)
      [] r.op = "HostLogin"          -> HostLogin(r.n)
      [] r.op = "TsNeedsLogin"       -> TsNeedsLogin(r.n)
      [] r.op = "TsURL"              -> TsURL(r.n)
      [] r.op = "Approve"            -> Approve(r.n)
      [] r.op = "TsFinished"         -> TsFinished(r.n)
      [] r.op = "TsKeyAccepted"      -> TsKeyAccepted(r.n)
      [] r.op = "TsKeyAcceptedQuiet" -> TsKeyAcceptedQuiet(r.n)
      [] r.op = "TsStarting"         -> TsStarting(r.n)
      [] r.op = "TsRunning"          -> TsRunning(r.n)
      [] r.op = "TsNetMap"           -> TsNetMap(r.n)
      [] r.op = "TsPollClosed"       -> TsPollClosed(r.n)
      [] r.op = "TsExpire"           -> TsExpire(r.n)
      [] r.op = "TsExpiredState"     -> TsExpiredState(r.n)
      [] r.op = "TsRefuse"           -> TsRefuse(r.n, r.cls)
      [] r.op = "TsRateLimited"      -> TsRateLimited(r.n)
      [] r.op = "TsLogDown"          -> TsLogDown(r.n)
      [] r.op = "TsCertFail"         -> TsCertFail(r.n)
      [] r.op = "CtlDown"            -> CtlDown
      [] r.op = "CtlUp"              -> CtlUp
      [] OTHER                       -> FALSE

TraceInit == l = 1 /\ Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Do(Trace[l + 1])
    /\ Pin(Trace[l + 1])

\* The end of the trace: stutter, so that a trace read to its end is no deadlock.
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars
=============================================================================
