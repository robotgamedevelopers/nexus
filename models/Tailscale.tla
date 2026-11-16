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
(* (src/tailscale/lifecycle/walk), which plays this module's environment   *)
(* and host step by step, is a behaviour of this module: every event the   *)
(* code is given is one an action here produces in that state.            *)
(*                                                                         *)
(* Around the nodes, the environment the host lives in:                    *)
(*   the host     starts a node on a state directory (refused while        *)
(*                another node holds it), with an auth key or without,     *)
(*                and with a control URL whose certificate is trusted or   *)
(*                not; closes it; and asks for a new login when there is   *)
(*                nothing to sign in with (expired, failed, or a sign-in   *)
(*                with no URL);                                            *)
(*   tsnet        reports its state (a restarted or keyed node may go      *)
(*                straight to running), asks control for a sign-in URL,    *)
(*                finishes a login once the person approved or control     *)
(*                accepted the auth key, receives and applies a network    *)
(*                map, sees its map poll close while control is down, and  *)
(*                logs the errors it meets (a certificate control's server *)
(*                shows that is not trusted, at any moment, for good);     *)
(*   control      goes down and comes back (MaxOutages times), expires a   *)
(*                node's key (MaxExpiries times), refuses a login          *)
(*                (MaxRefusals times), or asks a node to wait;             *)
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
    StartKeys,     \* whether a start may have an auth key: a subset of BOOLEAN
    StartCerts,    \* whether a start's control server may show an untrusted certificate: a subset of BOOLEAN
    RefusalClasses, \* the classes of refusal control gives: a subset of ErrClasses
    Broken         \* the witnesses' faults, a subset of Faults

Faults == {"ready-without-peers", "url-kept", "closed-runs", "table-ignores", "no-relogin", "poll-ignored",
           "cert-ignored"}
ASSUME Broken \subseteq Faults /\ StartKeys \subseteq BOOLEAN /\ StartCerts \subseteq BOOLEAN

Nodes == 1..N
Dirs == 1..D
NoNode == 0
NoDir == 0

Backends == {"NoState", "NeedsLogin", "NeedsMachineAuth", "Stopped", "Starting", "Running"}
Refusals == {"none", "auth-key", "tags", "refused", "certificate", "logging"}
\* The classes of control's refusals in an error message (ipn.Notify.ErrMessage).
ErrClasses == {"auth-key", "tags", "refused", "logging"}
ASSUME RefusalClasses \subseteq ErrClasses
Phases == {"not-started", "starting", "needs-login", "running", "ready", "expired", "stopped", "failed"}
Reasons == Refusals \cup {"control-unreachable", "machine-approval", "closed", "control-stopped"}

\* A node's facts (lifecycle.Node without its texts and generation).
Fresh == [started |-> FALSE, closed |-> FALSE, backend |-> "NoState", pend |-> FALSE, peers |-> FALSE,
          kexp |-> FALSE, refusal |-> "none", down |-> FALSE]

Facts == [started : BOOLEAN, closed : BOOLEAN, backend : Backends, pend : BOOLEAN, peers : BOOLEAN,
          kexp : BOOLEAN, refusal : Refusals, down : BOOLEAN]

\* An event (lifecycle.Event): k its kind; s a backend state; has whether a URL is given; cls the class of control's
\* message (err: a refusal or "ratelimit"; logged: "certificate" or "network"); flag for poll (TRUE: a network map
\* received and applied; FALSE: the map poll closed) and self (the key expired).
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
           [] e.k = "poll"     -> IF ~e.flag THEN [r EXCEPT !.down = TRUE]
                                  ELSE IF r.kexp \/ "poll-ignored" \in Broken THEN r
                                  ELSE [r EXCEPT !.peers = TRUE, !.down = FALSE]
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

\* lifecycle.Node.ControlDown, nts_status.control_down.
ControlDown(r) == r.started /\ ~r.closed /\ r.down

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
    keyed,     \* [Nodes -> BOOLEAN]: the node was started with an auth key control accepts
    badCert,   \* [Nodes -> BOOLEAN]: the node's control server shows a certificate that is not trusted
    wantURL,   \* [Nodes -> BOOLEAN]: a login is asked for (tsnet at start, or the host's Login), not yet answered
    approved,  \* [Nodes -> BOOLEAN]: the person approved the URL shown
    ctl,       \* "up" or "down"
    outages, expiries, refusals

vars == <<node, dirOf, holder, enrolled, keyDead, keyed, badCert, wantURL, approved, ctl, outages, expiries,
          refusals>>

TypeOK ==
    /\ node \in [Nodes -> Facts]
    /\ dirOf \in [Nodes -> Dirs \cup {NoDir}]
    /\ holder \in [Dirs -> Nodes \cup {NoNode}]
    /\ enrolled \in [Dirs -> BOOLEAN] /\ keyDead \in [Dirs -> BOOLEAN]
    /\ keyed \in [Nodes -> BOOLEAN] /\ badCert \in [Nodes -> BOOLEAN]
    /\ wantURL \in [Nodes -> BOOLEAN] /\ approved \in [Nodes -> BOOLEAN]
    /\ ctl \in {"up", "down"}
    /\ outages \in 0..MaxOutages /\ expiries \in 0..MaxExpiries /\ refusals \in 0..MaxRefusals

Init ==
    /\ node = [n \in Nodes |-> Fresh]
    /\ dirOf = [n \in Nodes |-> NoDir]
    /\ holder = [d \in Dirs |-> NoNode]
    /\ enrolled = [d \in Dirs |-> FALSE] /\ keyDead = [d \in Dirs |-> FALSE]
    /\ keyed = [n \in Nodes |-> FALSE] /\ badCert = [n \in Nodes |-> FALSE]
    /\ wantURL = [n \in Nodes |-> FALSE] /\ approved = [n \in Nodes |-> FALSE]
    /\ ctl = "up" /\ outages = 0 /\ expiries = 0 /\ refusals = 0

Live(n) == node[n].started /\ ~node[n].closed
Dir(n) == dirOf[n]

\* One event to node n's facts.
Apply(n, e) == node' = [node EXCEPT ![n] = Step(@, e)]

------------------------------------------------------------------------------
\* The host.

\* A start holds the directory (lifecycle.Table.Reserve), refused while another node holds it; with an auth key (k)
\* tsnet asks for no sign-in URL, without one it asks by itself if the directory needs one; c: the control server's
\* certificate is not trusted.
HostStart(n, d, k, c) ==
    /\ ~node[n].started
    /\ holder[d] = NoNode \/ "table-ignores" \in Broken
    /\ holder' = [holder EXCEPT ![d] = n]
    /\ dirOf' = [dirOf EXCEPT ![n] = d]
    /\ Apply(n, Ev("start", "", FALSE, "", FALSE))
    /\ keyed' = [keyed EXCEPT ![n] = k]
    /\ badCert' = [badCert EXCEPT ![n] = c]
    /\ wantURL' = [wantURL EXCEPT ![n] = ~k]
    /\ approved' = [approved EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<enrolled, keyDead, ctl, outages, expiries, refusals>>

HostClose(n) ==
    /\ Live(n)
    /\ Apply(n, Ev("close", "", FALSE, "", FALSE))
    /\ holder' = [holder EXCEPT ![Dir(n)] = IF @ = n THEN NoNode ELSE @]
    /\ UNCHANGED <<dirOf, enrolled, keyDead, keyed, badCert, wantURL, approved, ctl, outages, expiries, refusals>>

\* The host asks for a new login (nts_login) when there is nothing to sign in with.
Stuck(n) ==
    \/ Phase(node[n]) \in {"expired", "failed"}
    \/ Phase(node[n]) = "needs-login" /\ ~Shown(node[n]) /\ Reason(node[n]) # "machine-approval"

HostLogin(n) ==
    /\ Live(n) /\ Stuck(n)
    /\ Apply(n, Ev("login", "", FALSE, "", FALSE))
    /\ wantURL' = [wantURL EXCEPT ![n] = "no-relogin" \notin Broken]
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, keyed, badCert, approved, ctl, outages, expiries, refusals>>

------------------------------------------------------------------------------
\* tsnet, control and the person.

Enrolled(n) == enrolled[Dir(n)] /\ ~keyDead[Dir(n)]

\* Control can be talked to: it is up, and its certificate is trusted.
Reach(n) == ctl = "up" /\ (~badCert[n] \/ "cert-ignored" \in Broken)

\* Everything but the node's facts unchanged.
Keep == UNCHANGED <<dirOf, holder, enrolled, keyDead, keyed, badCert, wantURL, approved, ctl, outages, expiries,
                    refusals>>

\* A node with no accepted keys needs a login (tsnet decides it from its own state, without control).
TsNeedsLogin(n) ==
    /\ Live(n) /\ ~Enrolled(n) /\ node[n].backend = "NoState"
    /\ Apply(n, Ev("backend", "NeedsLogin", FALSE, "", FALSE))
    /\ Keep

TsURL(n) ==
    /\ Live(n) /\ Reach(n) /\ wantURL[n] /\ node[n].backend = "NeedsLogin" /\ ~node[n].pend
    /\ Apply(n, Ev("url", "", TRUE, "", FALSE))
    /\ wantURL' = [wantURL EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, keyed, badCert, approved, ctl, outages, expiries, refusals>>

Approve(n) ==
    /\ Live(n) /\ Shown(node[n]) /\ ~approved[n]
    /\ approved' = [approved EXCEPT ![n] = TRUE]
    /\ UNCHANGED <<node, dirOf, holder, enrolled, keyDead, keyed, badCert, wantURL, ctl, outages, expiries,
                   refusals>>

TsFinished(n) ==
    /\ Live(n) /\ Reach(n) /\ approved[n]
    /\ Apply(n, Ev("finished", "", FALSE, "", FALSE))
    /\ enrolled' = [enrolled EXCEPT ![Dir(n)] = TRUE]
    /\ keyDead' = [keyDead EXCEPT ![Dir(n)] = FALSE]
    /\ approved' = [approved EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<dirOf, holder, keyed, badCert, wantURL, ctl, outages, expiries, refusals>>

\* Control accepts the auth key of a node on a directory it has not enrolled: with tsnet's login-finished message
\* (TsKeyAccepted) or without one (TsKeyAcceptedQuiet).
KeyAcceptable(n) ==
    /\ Live(n) /\ Reach(n) /\ keyed[n] /\ ~enrolled[Dir(n)] /\ node[n].backend \in {"NoState", "NeedsLogin"}

TsKeyAccepted(n) ==
    /\ KeyAcceptable(n)
    /\ Apply(n, Ev("finished", "", FALSE, "", FALSE))
    /\ enrolled' = [enrolled EXCEPT ![Dir(n)] = TRUE]
    /\ UNCHANGED <<dirOf, holder, keyDead, keyed, badCert, wantURL, approved, ctl, outages, expiries, refusals>>

TsKeyAcceptedQuiet(n) ==
    /\ KeyAcceptable(n)
    /\ enrolled' = [enrolled EXCEPT ![Dir(n)] = TRUE]
    /\ UNCHANGED <<node, dirOf, holder, keyDead, keyed, badCert, wantURL, approved, ctl, outages, expiries,
                   refusals>>

\* An enrolled node brings its tunnel up from its own state: by way of starting, or (a restart, an accepted key)
\* straight to running.
TsStarting(n) ==
    /\ Live(n) /\ Enrolled(n) /\ node[n].backend \in {"NoState", "NeedsLogin"}
    /\ Apply(n, Ev("backend", "Starting", FALSE, "", FALSE))
    /\ Keep

TsRunning(n) ==
    /\ Live(n) /\ Enrolled(n) /\ node[n].backend \in {"NoState", "Starting"}
    /\ Apply(n, Ev("backend", "Running", FALSE, "", FALSE))
    /\ Keep

\* A network map from control is received and applied (and control is reachable again).
TsNetMap(n) ==
    /\ Live(n) /\ Reach(n) /\ Enrolled(n) /\ node[n].backend \in {"NoState", "Starting", "Running"}
    /\ ~node[n].peers \/ node[n].down
    /\ Apply(n, Ev("poll", "", FALSE, "", TRUE))
    /\ Keep

\* The map poll closes while control is down.
TsPollClosed(n) ==
    /\ Live(n) /\ ctl = "down" /\ node[n].peers /\ ~node[n].down
    /\ Apply(n, Ev("poll", "", FALSE, "", FALSE))
    /\ Keep

\* Control expires the key: the self node says so, then tsnet needs a login (with no URL: it asks for none itself).
TsExpire(n) ==
    /\ Live(n) /\ expiries < MaxExpiries /\ Enrolled(n) /\ node[n].backend = "Running"
    /\ expiries' = expiries + 1
    /\ keyDead' = [keyDead EXCEPT ![Dir(n)] = TRUE]
    /\ Apply(n, Ev("self", "", FALSE, "", TRUE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyed, badCert, wantURL, approved, ctl, outages, refusals>>

TsExpiredState(n) ==
    /\ Live(n) /\ keyDead[Dir(n)] /\ node[n].backend \in {"NoState", "Starting", "Running"}
    /\ Apply(n, Ev("backend", "NeedsLogin", FALSE, "", FALSE))
    /\ Keep

\* Control refuses the login, with a class of refusal (tsnet keeps retrying; the host sees failed until it asks
\* again).
TsRefuse(n, cls) ==
    /\ Live(n) /\ Reach(n) /\ refusals < MaxRefusals /\ ~Enrolled(n)
    /\ node[n].backend \in {"NoState", "NeedsLogin"}
    /\ refusals' = refusals + 1
    /\ Apply(n, Ev("err", "", FALSE, cls, FALSE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, keyed, badCert, wantURL, approved, ctl, outages, expiries>>

\* Control asks a node that registers to wait (a rate limit): reachable, but not answering yet. Counted with the
\* refusals.
TsRateLimited(n) ==
    /\ Live(n) /\ Reach(n) /\ refusals < MaxRefusals /\ ~Enrolled(n)
    /\ node[n].backend \in {"NoState", "NeedsLogin"} /\ ~node[n].down
    /\ refusals' = refusals + 1
    /\ Apply(n, Ev("err", "", FALSE, "ratelimit", FALSE))
    /\ UNCHANGED <<dirOf, holder, enrolled, keyDead, keyed, badCert, wantURL, approved, ctl, outages, expiries>>

\* While control is down, tsnet logs the error it gets.
TsLogDown(n) ==
    /\ Live(n) /\ ctl = "down" /\ ~node[n].down
    /\ Apply(n, Ev("logged", "", FALSE, "network", FALSE))
    /\ Keep

\* The control server's certificate is not trusted: tsnet logs it, at its first try and after every new login,
\* and gets no further with control for as long as the node runs.
TsCertFail(n) ==
    /\ Live(n) /\ ctl = "up" /\ badCert[n] /\ node[n].refusal # "certificate"
    /\ Apply(n, Ev("logged", "", FALSE, "certificate", FALSE))
    /\ Keep

CtlDown ==
    /\ ctl = "up" /\ outages < MaxOutages
    /\ ctl' = "down" /\ outages' = outages + 1
    /\ UNCHANGED <<node, dirOf, holder, enrolled, keyDead, keyed, badCert, wantURL, approved, expiries, refusals>>

CtlUp ==
    /\ ctl = "down" /\ ctl' = "up"
    /\ UNCHANGED <<node, dirOf, holder, enrolled, keyDead, keyed, badCert, wantURL, approved, outages, expiries,
                   refusals>>

Env(n) ==
    \/ TsNeedsLogin(n) \/ TsURL(n) \/ Approve(n) \/ TsFinished(n) \/ TsKeyAccepted(n) \/ TsKeyAcceptedQuiet(n)
    \/ TsStarting(n) \/ TsRunning(n) \/ TsNetMap(n) \/ TsPollClosed(n) \/ TsExpire(n) \/ TsExpiredState(n)
    \/ \E cls \in RefusalClasses : TsRefuse(n, cls)
    \/ TsRateLimited(n) \/ TsLogDown(n) \/ TsCertFail(n)

Next ==
    \/ \E n \in Nodes, d \in Dirs, k \in StartKeys, c \in StartCerts : HostStart(n, d, k, c)
    \/ \E n \in Nodes : HostClose(n) \/ HostLogin(n) \/ Env(n)
    \/ CtlDown \/ CtlUp

\* Fairness: assumptions about the world outside the shim, not things the shim does. In plain words: tsnet keeps
\* working (each of its steps that stays possible is taken); control, once down, comes back up; the person approves
\* a sign-in URL that stays shown; and the host calls nts_login whenever the node stays expired, failed, or waiting
\* with no URL. Nothing forces the host to start or close a node, control to go down, expire a key, refuse a login or
\* ask a node to wait, or the certificate to be trusted.
Fairness ==
    /\ \A n \in Nodes :
        /\ WF_vars(TsNeedsLogin(n)) /\ WF_vars(TsURL(n)) /\ WF_vars(Approve(n)) /\ WF_vars(TsFinished(n))
        /\ WF_vars(TsKeyAccepted(n)) /\ WF_vars(TsStarting(n)) /\ WF_vars(TsRunning(n)) /\ WF_vars(TsNetMap(n))
        /\ WF_vars(TsExpiredState(n)) /\ WF_vars(TsCertFail(n)) /\ WF_vars(HostLogin(n))
    /\ WF_vars(CtlUp)

Spec == Init /\ [][Next]_vars /\ Fairness

------------------------------------------------------------------------------
\* Properties.

\* Ready means running and a network map received and applied since this start or sign-in.
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

\* A node whose control server's certificate is not trusted is never ready: it gets no map.
CertNeverReady == \A n \in Nodes : Live(n) /\ badCert[n] => Phase(node[n]) # "ready"

\* A started node that is never closed, whose control server's certificate is trusted, is ready in the end, and again
\* after every expiry, outage or refusal (each bounded by its Max), under Fairness: that is, only if the person
\* approves, the host calls nts_login, and control comes back up.
GetsReady == \A n \in Nodes : (Live(n) /\ ~badCert[n]) ~> (Phase(node[n]) = "ready" \/ node[n].closed)

\* A node whose certificate is not trusted ends failed with the reason certificate, for as long as it runs.
CertFails == \A n \in Nodes :
    (Live(n) /\ badCert[n]) ~> ((Phase(node[n]) = "failed" /\ Reason(node[n]) = "certificate") \/ node[n].closed)
=============================================================================
