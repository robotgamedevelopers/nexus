---------------------------- MODULE TailscaleTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real lifecycle code  *)
(* (src/tailscale/lifecycle/lifecycle.go: Step, derive and Table, the      *)
(* code the shim's watch runs for every node) is a behaviour of            *)
(* Tailscale.tla.                                                          *)
(*                                                                         *)
(* The walk (src/tailscale/lifecycle/walk) drives TraceN nodes on TraceD   *)
(* state directories with a seeded random walk of starts (each held in a   *)
(* lifecycle.Table, or refused), closes, and events of every kind with the *)
(* texts tsnet gives (refusals, certificate and network errors, sign-in    *)
(* URLs), in any order, and writes TailscaleTraceData.tla beside this      *)
(* module: Trace, one record per step, with the operation, the node, the   *)
(* directory, the event, and after it the code's facts, its state, its     *)
(* reason, whether it shows a URL, and the table's holders.                *)
(*                                                                         *)
(* Each step takes the step of Tailscale.tla the record names and pins the *)
(* next state to the record. A step the spec cannot take to that state has *)
(* no successor, which TLC reports as a deadlock. The invariants of        *)
(* Tailscale.tla are checked on the trace as well.                         *)
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
    /\ holder' = r.holder

Do(r) ==
    CASE r.op = "start" ->
            /\ ~node[r.n].started /\ holder[r.d] = NoNode
            /\ holder' = [holder EXCEPT ![r.d] = r.n]
            /\ dirOf' = [dirOf EXCEPT ![r.n] = r.d]
            /\ Apply(r.n, Ev("start", "", FALSE, "", FALSE))
      [] r.op = "refused" ->
            /\ ~node[r.n].started /\ holder[r.d] # NoNode
            /\ UNCHANGED <<node, holder, dirOf>>
      [] r.op = "close" ->
            /\ Live(r.n)
            /\ Apply(r.n, Ev("close", "", FALSE, "", FALSE))
            /\ holder' = [holder EXCEPT ![Dir(r.n)] = IF @ = r.n THEN NoNode ELSE @]
            /\ UNCHANGED dirOf
      [] r.op = "event" ->
            /\ node[r.n].started /\ r.e.k \in Kinds \ {"start", "close"}
            /\ Apply(r.n, r.e)
            /\ UNCHANGED <<holder, dirOf>>
      [] OTHER -> FALSE

TraceInit == l = 1 /\ Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Do(Trace[l + 1])
    /\ Pin(Trace[l + 1])
    /\ UNCHANGED <<enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

\* The end of the trace: stutter, so that a trace read to its end is no deadlock.
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars
=============================================================================
