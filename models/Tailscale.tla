------------------------------ MODULE Tailscale ------------------------------
(***************************************************************************)
(* The Tailscale shim's node lifecycle as the host sees it                 *)
(* (src/tailscale/nts.h, NTS_STATE_*; docs/tailscale.md, layer 1).         *)
(*                                                                         *)
(* The code is src/tailscale/lifecycle/lifecycle.go: Step takes a node's   *)
(* facts and one event and gives the facts after it, and the state is     *)
(* derived from the facts. Step and Phase here are the same function, on   *)
(* the facts with their texts left out (a URL is "present", a refusal is   *)
(* its reason). TailscaleTrace.tla checks that the code's walk             *)
(* (src/tailscale/lifecycle/walk) is a behaviour of this module.           *)
(*                                                                         *)
(* Around the nodes, the environment the host lives in:                    *)
(*   the host     starts a node on a state directory (refused while        *)
(*                another node holds it), closes it, and asks for a new    *)
(*                login when there is nothing to sign in with (expired,    *)
(*                failed, or a sign-in with no URL);                       *)
(*   tsnet        reports its state, asks control for a sign-in URL,      *)
(*                finishes a login once the person approved, brings the    *)
(*                tunnel up, receives the peer map, and logs the errors    *)
(*                control gives it;                                        *)
(*   control      goes down and comes back (MaxOutages times), expires a   *)
(*                node's key (MaxExpiries times), refuses a login          *)
(*                (MaxRefusals times);                                     *)
(*   the person   approves a sign-in URL the host shows.                   *)
(* What a state directory keeps across nodes: whether it is enrolled (its  *)
(* keys accepted by control) and whether its key has expired.              *)
(*                                                                         *)
(* Broken names the reversed witnesses' faults (models/witnesses, WTs);    *)
(* every real instance sets it to {}.                                      *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    N,             \* node slots: the nodes the host starts, each once (a handle is never reused)
    D,             \* state directories
    MaxOutages,    \* how many times control goes down
    MaxExpiries,   \* how many times control expires a key
    MaxRefusals,   \* how many times control refuses a login
    Broken         \* the witnesses' faults, a subset of Faults

Faults == {"ready-without-peers", "url-kept", "closed-runs", "table-ignores", "no-relogin", "poll-ignored"}
ASSUME Broken \subseteq Faults

Nodes == 1..N
Dirs == 1..D
NoNode == 0
NoDir == 0

Backends == {"NoState", "NeedsLogin", "NeedsMachineAuth", "Stopped", "Starting", "Running"}
Refusals == {"none", "auth-key", "tags", "refused", "certificate", "logging"}
Phases == {"not-started", "starting", "needs-login", "running", "ready", "expired", "stopped", "failed"}
Reasons == Refusals \cup {"control-unreachable", "machine-approval", "closed", "control-stopped"}

\* A node's facts (lifecycle.Node without its texts and generation).
Fresh == [started |-> FALSE, closed |-> FALSE, backend |-> "NoState", pend |-> FALSE, peers |-> FALSE,
          kexp |-> FALSE, refusal |-> "none", down |-> FALSE]

Facts == [started : BOOLEAN, closed : BOOLEAN, backend : Backends, pend : BOOLEAN, peers : BOOLEAN,
          kexp : BOOLEAN, refusal : Refusals, down : BOOLEAN]

\* An event (lifecycle.Event): k its kind; s a backend state; has whether a URL is given; cls the class of control's
\* message (err: a refusal or "ratelimit"; logged: "certificate" or "network"); flag for poll and self.
Ev(k, s, has, cls, flag) == [k |-> k, s |-> s, has |-> has, cls |-> cls, flag |-> flag]
Kinds == {"start", "close", "login", "backend", "url", "finished", "err", "logged", "poll", "self"}

\* lifecycle.Step.
Step(r, e) ==
    IF r.closed THEN r
    ELSE CASE e.k = "start"    -> [Fresh EXCEPT !.started = TRUE]
           [] e.k = "close"    -> [r EXCEPT !.closed = TRUE, !.pend = FALSE]
           [] e.k = "login"    -> [r EXCEPT !.pend = FALSE, !.refusal = "none", !.peers = FALSE]
           [] e.k = "backend"  -> [r EXCEPT !.backend = e.s,
                                    !.peers = IF e.s \in {"NeedsLogin", "NeedsMachineAuth"} THEN FALSE ELSE @,
                                    !.refusal = IF e.s = "Running" THEN "none" ELSE @,
                                    !.pend = IF e.s = "Running" /\ "url-kept" \notin Broken THEN FALSE ELSE @]
           [] e.k = "url"      -> [r EXCEPT !.pend = e.has, !.down = IF e.has THEN FALSE ELSE @]
           [] e.k = "finished" -> [r EXCEPT !.pend = IF "url-kept" \in Broken THEN @ ELSE FALSE, !.refusal = "none",
                                    !.kexp = FALSE, !.down = FALSE]
           [] e.k = "err"      -> IF e.cls = "ratelimit" THEN [r EXCEPT !.down = TRUE]
                                  ELSE [r EXCEPT !.down = FALSE, !.refusal = e.cls]
           [] e.k = "logged"   -> IF e.cls = "certificate" THEN [r EXCEPT !.refusal = "certificate"]
                                  ELSE [r EXCEPT !.down = TRUE]
           [] e.k = "poll"     -> IF e.flag /\ "poll-ignored" \notin Broken THEN [r EXCEPT !.peers = TRUE, !.down = FALSE]
                                  ELSE r
           [] e.k = "self"     -> [r EXCEPT !.kexp = e.flag, !.peers = IF e.flag THEN FALSE ELSE @]
           [] OTHER            -> r

\* lifecycle.derive: the host's state, its reason, and whether the sign-in URL is shown.
Phase(r) ==
    CASE r.closed /\ "closed-runs" \notin Broken -> "stopped"
      [] ~r.started                -> "not-started"
      [] r.refusal # "none"        -> "failed"
      [] r.backend = "NeedsLogin"  -> IF r.pend THEN "needs-login" ELSE IF r.kexp THEN "expired" ELSE "needs-login"
      [] r.backend = "NeedsMachineAuth" -> "needs-login"
      [] r.backend = "Running"     -> IF r.peers \/ "ready-without-peers" \in Broken THEN "ready" ELSE "running"
      [] r.backend = "Stopped"     -> "stopped"
      [] OTHER                     -> "starting"

Reason(r) ==
    CASE r.closed                  -> "closed"
      [] ~r.started                -> "none"
      [] r.refusal # "none"        -> r.refusal
      [] r.backend = "NeedsMachineAuth" -> "machine-approval"
      [] r.backend = "Stopped"     -> "control-stopped"
      [] r.down                    -> "control-unreachable"
      [] OTHER                     -> "none"

Shown(r) ==
    /\ r.started /\ ~r.closed /\ r.refusal = "none" /\ r.pend
    /\ \/ r.backend = "NeedsLogin"
       \/ "url-kept" \in Broken /\ r.backend = "Running"

VARIABLES
    node,      \* [Nodes -> Facts]
    dirOf,     \* the directory each node was started on, or NoDir
    holder,    \* [Dirs -> Nodes \cup {NoNode}]: lifecycle.Table, the node holding each directory
    enrolled,  \* [Dirs -> BOOLEAN]: control accepted the directory's keys
    keyDead,   \* [Dirs -> BOOLEAN]: control expired the directory's key
    wantURL,   \* [Nodes -> BOOLEAN]: a login is asked for (tsnet at start, or the host's Login), not yet answered
    approved,  \* [Nodes -> BOOLEAN]: the person approved the URL shown
    ctl,       \* "up" or "down"
    outages, expiries, refusals

vars == <<node, dirOf, holder, enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

TypeOK ==
    /\ node \in [Nodes -> Facts]
    /\ dirOf \in [Nodes -> Dirs \cup {NoDir}]
    /\ holder \in [Dirs -> Nodes \cup {NoNode}]
    /\ enrolled \in [Dirs -> BOOLEAN] /\ keyDead \in [Dirs -> BOOLEAN]
    /\ wantURL \in [Nodes -> BOOLEAN] /\ approved \in [Nodes -> BOOLEAN]
    /\ ctl \in {"up", "down"}
    /\ outages \in 0..MaxOutages /\ expiries \in 0..MaxExpiries /\ refusals \in 0..MaxRefusals

Init ==
    /\ node = [n \in Nodes |-> Fresh]
    /\ dirOf = [n \in Nodes |-> NoDir]
    /\ holder = [d \in Dirs |-> NoNode]
    /\ enrolled = [d \in Dirs |-> FALSE] /\ keyDead = [d \in Dirs |-> FALSE]
    /\ wantURL = [n \in Nodes |-> FALSE] /\ approved = [n \in Nodes |-> FALSE]
    /\ ctl = "up" /\ outages = 0 /\ expiries = 0 /\ refusals = 0

Live(n) == node[n].started /\ ~node[n].closed
Dir(n) == dirOf[n]

\* One event to node n's facts.
Apply(n, e) == node' = [node EXCEPT ![n] = Step(@, e)]

------------------------------------------------------------------------------
\* The host.

\* A start holds the directory (lifecycle.Table.Reserve), refused while another node holds it; tsnet then asks for a
\* sign-in by itself if the directory needs one.
HostStart(n, d) ==
    /\ ~node[n].started
    /\ holder[d] = NoNode \/ "table-ignores" \in Broken
    /\ holder' = [holder EXCEPT ![d] = n]
    /\ dirOf' = [dirOf EXCEPT ![n] = d]
    /\ Apply(n, Ev("start", "", FALSE, "", FALSE))
    /\ wantURL' = [wantURL EXCEPT ![n] = TRUE]
    /\ approved' = [approved EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<enrolled, keyDead, ctl, outages, expiries, refusals>>

HostClose(n) ==
    /\ Live(n)
    /\ Apply(n, Ev("close", "", FALSE, "", FALSE))
    /\ holder' = [holder EXCEPT ![Dir(n)] = IF @ = n THEN NoNode ELSE @]
    /\ UNCHANGED <<dirOf, enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

\* The host asks for a new login (nts_login) when there is nothing to sign in with.
Stuck(n) ==
    \/ Phase(node[n]) \in {"expired", "failed"}
    \/ Phase(node[n]) = "needs-login" /\ ~Shown(node[n]) /\ Reason(node[n]) # "machine-approval"

HostLogin(n) ==
    /\ Live(n) /\ Stuck(n)
    /\ Apply(n, Ev("login", "", FALSE, "", FALSE))
    /\ wantURL' = [wantURL EXCEPT ![n] = "no-relogin" \notin Broken]
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, approved, ctl, outages, expiries, refusals>>

------------------------------------------------------------------------------
\* tsnet, control and the person.

Enrolled(n) == enrolled[Dir(n)] /\ ~keyDead[Dir(n)]

TsNeedsLogin(n) ==
    /\ Live(n) /\ ctl = "up" /\ ~Enrolled(n) /\ node[n].backend = "NoState"
    /\ Apply(n, Ev("backend", "NeedsLogin", FALSE, "", FALSE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

TsURL(n) ==
    /\ Live(n) /\ ctl = "up" /\ wantURL[n] /\ node[n].backend = "NeedsLogin" /\ ~node[n].pend
    /\ Apply(n, Ev("url", "", TRUE, "", FALSE))
    /\ wantURL' = [wantURL EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, approved, ctl, outages, expiries, refusals>>

Approve(n) ==
    /\ Live(n) /\ Shown(node[n]) /\ ~approved[n]
    /\ approved' = [approved EXCEPT ![n] = TRUE]
    /\ UNCHANGED <<node, dirOf, holder, enrolled, keyDead, wantURL, ctl, outages, expiries, refusals>>

TsFinished(n) ==
    /\ Live(n) /\ ctl = "up" /\ approved[n]
    /\ Apply(n, Ev("finished", "", FALSE, "", FALSE))
    /\ enrolled' = [enrolled EXCEPT ![Dir(n)] = TRUE]
    /\ keyDead' = [keyDead EXCEPT ![Dir(n)] = FALSE]
    /\ approved' = [approved EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<dirOf, holder, wantURL, ctl, outages, expiries, refusals>>

TsStarting(n) ==
    /\ Live(n) /\ ctl = "up" /\ Enrolled(n) /\ node[n].backend \in {"NoState", "NeedsLogin"}
    /\ Apply(n, Ev("backend", "Starting", FALSE, "", FALSE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

TsRunning(n) ==
    /\ Live(n) /\ ctl = "up" /\ Enrolled(n) /\ node[n].backend = "Starting"
    /\ Apply(n, Ev("backend", "Running", FALSE, "", FALSE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

\* The map poll is open: the peer map is in (and control is reachable again).
TsPoll(n) ==
    /\ Live(n) /\ ctl = "up" /\ Enrolled(n) /\ node[n].backend \in {"Starting", "Running"}
    /\ ~node[n].peers \/ node[n].down
    /\ Apply(n, Ev("poll", "", FALSE, "", TRUE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

\* Control expires the key: the self node says so, then tsnet needs a login (with no URL: it asks for none itself).
TsExpire(n) ==
    /\ Live(n) /\ expiries < MaxExpiries /\ Enrolled(n) /\ node[n].backend = "Running"
    /\ expiries' = expiries + 1
    /\ keyDead' = [keyDead EXCEPT ![Dir(n)] = TRUE]
    /\ Apply(n, Ev("self", "", FALSE, "", TRUE))
    /\ UNCHANGED <<dirOf, holder, enrolled, wantURL, approved, ctl, outages, refusals>>

TsExpiredState(n) ==
    /\ Live(n) /\ keyDead[Dir(n)] /\ node[n].backend \in {"NoState", "Starting", "Running"}
    /\ Apply(n, Ev("backend", "NeedsLogin", FALSE, "", FALSE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

\* Control refuses the login (tsnet keeps retrying; the host sees failed until it asks again).
TsRefuse(n) ==
    /\ Live(n) /\ ctl = "up" /\ refusals < MaxRefusals /\ ~Enrolled(n) /\ node[n].backend = "NeedsLogin"
    /\ refusals' = refusals + 1
    /\ Apply(n, Ev("err", "", FALSE, "refused", FALSE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, wantURL, approved, ctl, outages, expiries>>

\* While control is down, tsnet logs the error it gets.
TsLogDown(n) ==
    /\ Live(n) /\ ctl = "down" /\ ~node[n].down
    /\ Apply(n, Ev("logged", "", FALSE, "network", FALSE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, wantURL, approved, ctl, outages, expiries, refusals>>

CtlDown ==
    /\ ctl = "up" /\ outages < MaxOutages
    /\ ctl' = "down" /\ outages' = outages + 1
    /\ UNCHANGED <<node, dirOf, holder, enrolled, keyDead, wantURL, approved, expiries, refusals>>

CtlUp ==
    /\ ctl = "down" /\ ctl' = "up"
    /\ UNCHANGED <<node, dirOf, holder, enrolled, keyDead, wantURL, approved, outages, expiries, refusals>>

Env(n) ==
    \/ TsNeedsLogin(n) \/ TsURL(n) \/ Approve(n) \/ TsFinished(n) \/ TsStarting(n) \/ TsRunning(n) \/ TsPoll(n)
    \/ TsExpire(n) \/ TsExpiredState(n) \/ TsRefuse(n) \/ TsLogDown(n)

Next ==
    \/ \E n \in Nodes, d \in Dirs : HostStart(n, d)
    \/ \E n \in Nodes : HostClose(n) \/ HostLogin(n) \/ Env(n)
    \/ CtlDown \/ CtlUp

\* Fairness, in plain words: tsnet keeps working while control is up (each of its steps that stays possible is
\* taken); control, once down, comes back; the person approves a URL that stays shown; and the host asks for a new
\* login whenever the node stays expired, failed, or waiting with no URL. Nothing forces the host to start or close a
\* node, control to go down, expire a key or refuse a login.
Fairness ==
    /\ \A n \in Nodes :
        /\ WF_vars(TsNeedsLogin(n)) /\ WF_vars(TsURL(n)) /\ WF_vars(Approve(n)) /\ WF_vars(TsFinished(n))
        /\ WF_vars(TsStarting(n)) /\ WF_vars(TsRunning(n)) /\ WF_vars(TsPoll(n)) /\ WF_vars(TsExpiredState(n))
        /\ WF_vars(HostLogin(n))
    /\ WF_vars(CtlUp)

Spec == Init /\ [][Next]_vars /\ Fairness

------------------------------------------------------------------------------
\* Properties.

\* Ready means running and the peer map received since this start or sign-in.
ReadyHasPeers == \A n \in Nodes : Phase(node[n]) = "ready" => node[n].peers /\ node[n].backend = "Running"

\* A closed node never reports running (or ready).
ClosedNeverRunning == \A n \in Nodes : node[n].closed => Phase(node[n]) \notin {"running", "ready"}

\* The sign-in URL is shown in no state but needs-login.
URLOnlyInNeedsLogin == \A n \in Nodes : Shown(node[n]) => Phase(node[n]) = "needs-login"

\* At most one live node per state directory, and the table names it.
OneNodePerDir ==
    /\ \A n, m \in Nodes : n # m /\ Live(n) /\ Live(m) => Dir(n) # Dir(m)
    /\ \A n \in Nodes : Live(n) => holder[Dir(n)] = n

\* Every state the host sees is one of nts.h's, with one of its reasons.
StatesNamed == \A n \in Nodes : Phase(node[n]) \in Phases /\ Reason(node[n]) \in Reasons

\* A started node that is never closed is ready in the end, and again after every expiry, outage or refusal
\* (each bounded by its Max), under Fairness.
GetsReady == \A n \in Nodes : Live(n) ~> (Phase(node[n]) = "ready" \/ node[n].closed)
=============================================================================
