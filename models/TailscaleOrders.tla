--------------------------- MODULE TailscaleOrders ---------------------------
(***************************************************************************)
(* Orders of a node's facts recorded from the real shim against the test   *)
(* control server (tests/tailscale), each checked to be a behaviour of     *)
(* Tailscale.tla's Next, its environment included: not only of Step.       *)
(*                                                                         *)
(* Order is the facts node 1 shows, one after another, from its start;     *)
(* steps of Next that leave node 1 as it is (control, the person, another  *)
(* node) may come between. OrderNotReached is violated exactly when the    *)
(* model can produce the whole order, so each config expects it violated.  *)
(*   restart  a node on a directory enrolled earlier: running straight     *)
(*            from its own state, then ready once its map came;            *)
(*   authkey  a node with an auth key on a new directory: the same, with   *)
(*            no sign-in;                                                  *)
(*   cert     a control server whose certificate is not trusted: failed    *)
(*            with the reason certificate before anything else;            *)
(*   starting an interactive restart by way of starting.                   *)
(***************************************************************************)
EXTENDS Tailscale, Sequences

CONSTANTS Scenario

VARIABLE l

S == [Fresh EXCEPT !.started = TRUE]
Ready1 == [S EXCEPT !.backend = "Running", !.peers = TRUE]

Order ==
    CASE Scenario = "restart"  -> << S, [S EXCEPT !.backend = "Running"], Ready1 >>
      [] Scenario = "authkey"  -> << S, [S EXCEPT !.backend = "Running"], Ready1 >>
      [] Scenario = "cert"     -> << S, [S EXCEPT !.refusal = "certificate"] >>
      [] OTHER                 -> << S, [S EXCEPT !.backend = "Starting"], [S EXCEPT !.backend = "Running"], Ready1 >>

\* The restarts begin on a directory control enrolled before.
OInit ==
    /\ l = 0
    /\ node = [n \in Nodes |-> Fresh]
    /\ dirOf = [n \in Nodes |-> NoDir]
    /\ holder = [d \in Dirs |-> NoNode]
    /\ keyed = [n \in Nodes |-> FALSE] /\ badCert = [n \in Nodes |-> FALSE]
    /\ wantURL = [n \in Nodes |-> FALSE] /\ approved = [n \in Nodes |-> FALSE]
    /\ ctl = "up" /\ outages = 0 /\ expiries = 0 /\ refusals = 0
    /\ enrolled = [d \in Dirs |-> Scenario \in {"restart", "starting"}]
    /\ keyDead = [d \in Dirs |-> FALSE]

ONext ==
    /\ l < Len(Order)
    /\ Next
    /\ IF node'[1] = node[1] THEN l' = l ELSE node'[1] = Order[l + 1] /\ l' = l + 1

ODone == l = Len(Order) /\ UNCHANGED <<vars, l>>

OSpec == OInit /\ [][ONext \/ ODone]_<<vars, l>>

OrderNotReached == l < Len(Order)
=============================================================================
