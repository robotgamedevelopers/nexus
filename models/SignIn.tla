------------------------------- MODULE SignIn -------------------------------
(***************************************************************************)
(* In-app sign-in for the Claude harness (docs/secrets.md, "Sign-in"): how *)
(* an account with no login is signed in from inside nexus, with one       *)
(* browser sign-in, no paste and no AI ever seeing the URL, the code or    *)
(* the token.                                                              *)
(*                                                                         *)
(* The server marks an account needed when it has no login (its driver    *)
(* said no credential, or no token is stored): a flow is minted, its id    *)
(* durable and never reused, and offered in the state every window gets.  *)
(* A window the person has just shown (armed) claims the oldest offer it   *)
(* has not claimed. The first claim the server takes for a needed flow     *)
(* starts the unmodified `claude setup-token` for the account (started);   *)
(* any later claim, from a second window or the same one reconnected,     *)
(* starts nothing: it only takes the flow over when no window holds it.    *)
(* The process hands the sign-in URL to the server through nexus's `open`  *)
(* helper (url-sent); the server offers it to the claiming window alone,   *)
(* which opens it in the system browser and says so (waiting). After the   *)
(* person signs in, the browser is sent to the redirect: on the server's   *)
(* own machine Claude Code's listener takes it directly; on another        *)
(* machine the window's one-shot gate takes exactly one GET whose state    *)
(* matches the URL it opened, and the window sends its query up; the       *)
(* server replays it once, and only with the matching state, to the        *)
(* listener (callback-forwarded). The process prints the token, which the  *)
(* server writes straight into the one-chance store (captured): stored    *)
(* when the disk takes it, else held and written again, never dropped.     *)
(* The account is then signed in.                                          *)
(*                                                                         *)
(* A process that ends with no token ends its flow failed (cancelled when  *)
(* the person declined on the page); a flow not done by its deadline       *)
(* expires and its process is told to end, which it does a moment later   *)
(* (ending: SIGTERM, then SIGKILL after a grace); a token it prints in    *)
(* that moment is stored all the same (late). A window whose link ends     *)
(* lets go of the flows it held. Each ending leaves the account needed     *)
(* again: a new flow, which a window claims at the person's next show. A   *)
(* window binds one gate at a time: a second page that needs one waits     *)
(* while the first's waits for its callback. A server restart drops every  *)
(* flow and ends every process; a held token is lost with the server's     *)
(* memory (the one documented window).                                     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Clients,     \* the windows that may connect
    Remote,      \* those on another machine than the server: they forward the callback; the rest bind nothing
    Accounts,    \* the accounts a sign-in can be for
    Tokens,      \* the logins `claude setup-token` may print: opaque
    MaxFlows,    \* how many flows are minted in a behaviour, ids 1..MaxFlows
    MaxRestarts, \* how many times the server restarts
    MaxFailures, \* how many times the disk starts failing
    MaxLosses    \* how many times a signed-in account loses its login (its driver says no credential)

ASSUME Remote \subseteq Clients /\ MaxFlows >= 1 /\ MaxRestarts >= 0 /\ MaxFailures >= 0 /\ MaxLosses >= 0

VARIABLES
    next,       \* the next flow id: durable, never reused across restarts
    flow,       \* each id's flow: its state, account and the window that holds it (opener)
    proc,       \* each account's `claude setup-token`: what it has done, and the flow it serves
    procs,      \* ghost: how many processes are alive for each account
    cbs,        \* ghost: how many callbacks the server replayed to each flow's listener
    badcb,      \* ghost: the flows a callback was replayed to whose state did not match
    signedin,   \* each account's sign-in, as the server holds it
    losses,     \* how many logins were lost
    conn,       \* which windows have a link to the server
    armed,      \* the window was just shown by the person: its next claim may go
    view,       \* the offers each window last received in its state
    claimed,    \* the offers each window has claimed: never claimed again
    opened,     \* the offers each window has opened in the browser: never opened again
    browser,    \* the sign-in pages open in each window's browser, by flow id
    gate,       \* each window's one-shot callback gate: the flow it waits for (0: none)
    up,         \* the windows' messages in flight to the server, each delivered once
    file,       \* each account's 0600 login file
    held,       \* each account's token the server holds in memory while its write is retried
    disk,       \* whether a write of an account's file is durable now ("ok") or fails ("failing")
    failures,   \* how many times the disk started failing
    taken,      \* ghost: the token captured for each flow, which must never be dropped
    lost,       \* ghost: the flows whose held token went with a restart
    restarts,   \* how many times the server restarted
    said,       \* ghost: the token each flow's process printed, in time or late
    log         \* the lines the last step wrote to server.log

vars == <<next, flow, proc, procs, cbs, badcb, signedin, losses, conn, armed, view, claimed, opened, browser, gate, up,
          file, held, disk, failures, taken, lost, restarts, said, log>>
win == <<conn, armed, view, claimed, opened, browser, gate>>
srv == <<next, flow, proc, procs, cbs, badcb, signedin, losses>>
store == <<file, held, disk, failures, taken, lost, restarts, said>>

None == "none"
Ids == 1 .. MaxFlows
States == {"unused", "needed", "started", "url-sent", "waiting", "callback-forwarded", "captured", "failed",
           "cancelled", "expired", "dropped"}
Live == {"needed", "started", "url-sent", "waiting", "callback-forwarded"}
Running == {"started", "url-sent", "waiting", "callback-forwarded"}
\* running: started, no URL yet; url: the URL handed over, its listener waiting; got-code / got-denied: its listener took
\* the redirect, with a code or with the person's refusal. ending: its flow ended and it was told to end; it still runs
\* (and may print) until it does.
ProcStates == {"none", "running", "url", "got-code", "got-denied"}
NoProc == [st |-> "none", id |-> 0, ending |-> FALSE]
Min(S) == CHOOSE x \in S : \A y \in S : x <= y

LiveOf(a) == {i \in Ids : flow[i].acct = a /\ flow[i].st \in Live}
Held == "held"
\* What window c receives: the live flows, each with whether a window holds it (c itself, or "held" by another it
\* cannot name: no window learns another's claim token), and whether its URL is there (for the window holding it
\* alone, once the process gave it); never a token.
PubFor(c) == {[id |-> i, acct |-> flow[i].acct,
               opener |-> IF flow[i].opener = None THEN None ELSE IF flow[i].opener = c THEN c ELSE Held,
               url |-> (flow[i].st \in Running \ {"started"} /\ flow[i].opener = c)] :
              i \in {j \in Ids : flow[j].st \in Live}}

Init ==
    /\ next = 1
    /\ flow = [i \in Ids |-> [st |-> "unused", acct |-> None, opener |-> None, denied |-> FALSE]]
    /\ proc = [a \in Accounts |-> NoProc]
    /\ procs = [a \in Accounts |-> 0]
    /\ cbs = [i \in Ids |-> 0]
    /\ badcb = {}
    /\ signedin = [a \in Accounts |-> FALSE]
    /\ losses = 0
    /\ conn = [c \in Clients |-> TRUE]
    /\ armed = [c \in Clients |-> FALSE]
    /\ view = [c \in Clients |-> {}]
    /\ claimed = [c \in Clients |-> {}]
    /\ opened = [c \in Clients |-> {}]
    /\ browser = [c \in Clients |-> {}]
    /\ gate = [c \in Clients |-> 0]
    /\ up = {}
    /\ file = [a \in Accounts |-> None]
    /\ held = [a \in Accounts |-> None]
    /\ disk = "ok"
    /\ failures = 0
    /\ taken = [i \in Ids |-> None]
    /\ lost = {}
    /\ restarts = 0
    /\ said = [i \in Ids |-> None]
    /\ log = {}

----------------------------------------------------------------------------
\* The server.

\* The account has no login and no flow (and no token held): a flow is minted, needed. B(a) is when that may be.
NeedWith(B(_), a) ==
    /\ ~signedin[a] /\ next <= MaxFlows /\ B(a)
    /\ flow' = [flow EXCEPT ![next] = [st |-> "needed", acct |-> a, opener |-> None, denied |-> FALSE]]
    /\ next' = next + 1
    /\ log' = {[id |-> next, event |-> "needed"]}
    /\ UNCHANGED <<proc, procs, cbs, badcb, signedin, losses, up>> /\ UNCHANGED win /\ UNCHANGED store

Need(a) == NeedWith(LAMBDA x : LiveOf(x) = {} /\ held[x] = None, a)

\* A signed-in account loses its login: its driver said no credential, or its year ended.
Lose(a) ==
    /\ signedin[a] /\ losses < MaxLosses
    /\ signedin' = [signedin EXCEPT ![a] = FALSE] /\ losses' = losses + 1 /\ log' = {}
    /\ UNCHANGED <<next, flow, proc, procs, cbs, badcb, up>> /\ UNCHANGED win /\ UNCHANGED store

\* No linked window reads sign-ins (each is from a build before them), so the account is asked for by a paste instead
\* (Hub::signInWindowsChanged, SignInDesk::fallBack): a needed flow, which no window holds and no process serves, ends
\* dropped. The paste itself is the Secrets model's; here the account simply stays without a login until a later need.
FallBack(i) ==
    /\ flow[i].st = "needed"
    /\ flow' = [flow EXCEPT ![i].st = "dropped"]
    /\ log' = {[id |-> i, event |-> "dropped"]}
    /\ UNCHANGED <<next, proc, procs, cbs, badcb, signedin, losses, up>> /\ UNCHANGED win /\ UNCHANGED store

\* A claim the server takes. For a needed flow it starts the account's process; for a running one no window holds, the
\* window takes it over and nothing starts; any other is late.
ClaimCorrect(m) ==
    LET i == m.id
        a == flow[i].acct
    IN IF flow[i].st = "needed"
       \* A process of an ended flow still ending is ended first (SignInRunner::start): one process for the account.
       THEN /\ flow' = [flow EXCEPT ![i].st = "started", ![i].opener = m.c]
            /\ proc' = [proc EXCEPT ![a] = [st |-> "running", id |-> i, ending |-> FALSE]]
            /\ procs' = IF proc[a].st = "none" THEN [procs EXCEPT ![a] = @ + 1] ELSE procs
            /\ log' = {[id |-> i, event |-> "started"]}
       ELSE IF flow[i].st \in Running /\ flow[i].opener = None
       THEN /\ flow' = [flow EXCEPT ![i].opener = m.c]
            /\ log' = {[id |-> i, event |-> "claimed"]}
            /\ UNCHANGED <<proc, procs>>
       ELSE /\ log' = {[id |-> i, event |-> "late"]}
            /\ UNCHANGED <<flow, proc, procs>>

\* The window that holds a flow opened its URL.
OpenedCorrect(m) ==
    IF flow[m.id].st = "url-sent" /\ flow[m.id].opener = m.c
    THEN /\ flow' = [flow EXCEPT ![m.id].st = "waiting"]
         /\ log' = {[id |-> m.id, event |-> "waiting"]}
    ELSE /\ log' = {[id |-> m.id, event |-> "late"]} /\ UNCHANGED flow

\* A callback the window's gate sent up: replayed to the listener once, from the window that holds the flow, and only
\* with the state of the URL it opened; any other is dropped.
CallbackCorrect(m) ==
    LET i == m.id
        a == flow[i].acct
    IN IF flow[i].st = "waiting" /\ flow[i].opener = m.c /\ m.match
       THEN /\ flow' = [flow EXCEPT ![i].st = "callback-forwarded", ![i].denied = ~m.ok]
            /\ cbs' = [cbs EXCEPT ![i] = @ + 1]
            /\ proc' = IF proc[a].id = i /\ proc[a].st = "url"
                       THEN [proc EXCEPT ![a].st = IF m.ok THEN "got-code" ELSE "got-denied"]
                       ELSE proc
            /\ log' = {[id |-> i, event |-> "forwarded"]}
            /\ UNCHANGED badcb
       ELSE /\ log' = {[id |-> i, event |-> "late"]} /\ UNCHANGED <<flow, cbs, proc, badcb>>

\* The server takes a window's message. C(m) is its handling of a claim, O(m) of an opened, B(m) of a callback.
ReceiveWith(C(_), O(_), B(_), m) ==
    /\ m \in up
    /\ up' = up \ {m}
    /\ CASE m.kind = "claim"    -> C(m) /\ UNCHANGED <<cbs, badcb>>
         [] m.kind = "opened"   -> O(m) /\ UNCHANGED <<proc, procs, cbs, badcb>>
         [] m.kind = "callback" -> B(m) /\ UNCHANGED procs
    /\ UNCHANGED <<next, signedin, losses>> /\ UNCHANGED win /\ UNCHANGED store

Receive(m) == ReceiveWith(ClaimCorrect, OpenedCorrect, CallbackCorrect, m)

\* The process hands its sign-in URL to the server, through nexus's `open` helper.
Url(a) ==
    /\ proc[a].st = "running" /\ flow[proc[a].id].st = "started"
    /\ proc' = [proc EXCEPT ![a].st = "url"]
    /\ flow' = [flow EXCEPT ![proc[a].id].st = "url-sent"]
    /\ log' = {[id |-> proc[a].id, event |-> "url"]}
    /\ UNCHANGED <<next, procs, cbs, badcb, signedin, losses, up>> /\ UNCHANGED win /\ UNCHANGED store

\* The token, captured from the process's output straight into the one-chance store: durable now, or held.
\* W(a, v) is what the store does with it.
WriteCorrect(a, v) ==
    IF disk = "ok"
    THEN /\ file' = [file EXCEPT ![a] = v] /\ signedin' = [signedin EXCEPT ![a] = TRUE] /\ UNCHANGED held
    ELSE /\ held' = [held EXCEPT ![a] = v] /\ UNCHANGED <<file, signedin>>

\* The process prints its token. In time (its flow running), the flow is captured. Late (the flow ended while the
\* process, told to end, still ran: its deadline passed during the callback, or in the grace after), the token is
\* stored all the same, and a needed flow for the account is dropped: the person signed in.
PrintWith(W(_, _), a, v) ==
    /\ proc[a].st = "got-code"
    /\ LET i == proc[a].id IN
       /\ IF flow[i].st \in Running
          THEN /\ flow' = [flow EXCEPT ![i].st = "captured", ![i].opener = None]
               /\ log' = {[id |-> i, event |-> "captured"]}
          ELSE /\ flow' = [j \in Ids |-> IF flow[j].acct = a /\ flow[j].st = "needed"
                                          THEN [flow[j] EXCEPT !.st = "dropped"] ELSE flow[j]]
               /\ log' = {[id |-> i, event |-> "late"]} \cup
                         {[id |-> j, event |-> "dropped"] : j \in {k \in Ids : flow[k].acct = a /\ flow[k].st = "needed"}}
       /\ taken' = [taken EXCEPT ![i] = v]
       /\ said' = [said EXCEPT ![i] = v]
    /\ proc' = [proc EXCEPT ![a] = NoProc] /\ procs' = [procs EXCEPT ![a] = @ - 1]
    /\ W(a, v)
    /\ UNCHANGED <<next, cbs, badcb, losses, up, disk, failures, lost, restarts>> /\ UNCHANGED win

Print(a, v) == PrintWith(WriteCorrect, a, v)

\* The process ends with no token: cancelled when the server forwarded the person's refusal on the page (the window's
\* gate sent it), failed for anything else. A refusal on the server's own machine goes straight to the process's
\* listener, which the server does not see: that flow ends failed. A process told to end ends by End, not this.
Exit(a) ==
    /\ proc[a].st # "none" /\ ~proc[a].ending
    /\ LET i == proc[a].id IN
       /\ flow' = [flow EXCEPT ![i].st = IF flow[i].denied THEN "cancelled" ELSE "failed", ![i].opener = None]
       /\ log' = {[id |-> i, event |-> IF flow[i].denied THEN "cancelled" ELSE "failed"]}
    /\ proc' = [proc EXCEPT ![a] = NoProc] /\ procs' = [procs EXCEPT ![a] = @ - 1]
    /\ UNCHANGED <<next, cbs, badcb, signedin, losses, up>> /\ UNCHANGED win /\ UNCHANGED store

\* A running flow's deadline passes: it expires, and its process is told to end (K(i)); it ends a step later (End).
TimeoutWith(K(_), i) ==
    /\ flow[i].st \in Running
    /\ flow' = [flow EXCEPT ![i].st = "expired", ![i].opener = None]
    /\ K(i)
    /\ log' = {[id |-> i, event |-> "expired"]}
    /\ UNCHANGED <<next, cbs, badcb, signedin, losses, up>> /\ UNCHANGED win /\ UNCHANGED store

Kill(i) ==
    LET a == flow[i].acct IN
    IF proc[a].id = i /\ proc[a].st # "none"
    THEN proc' = [proc EXCEPT ![a].ending = TRUE] /\ UNCHANGED procs
    ELSE UNCHANGED <<proc, procs>>

Timeout(i) == TimeoutWith(Kill, i)

\* A process told to end ends: by itself on SIGTERM, or by SIGKILL after the grace. Its flow ended already.
End(a) ==
    /\ proc[a].st # "none" /\ proc[a].ending
    /\ proc' = [proc EXCEPT ![a] = NoProc] /\ procs' = [procs EXCEPT ![a] = @ - 1]
    /\ log' = {}
    /\ UNCHANGED <<next, flow, cbs, badcb, signedin, losses, up>> /\ UNCHANGED win /\ UNCHANGED store

\* A held token is written again: the disk takes it, the account is signed in, and the memory of it is wiped.
Retry(a) ==
    /\ held[a] # None /\ disk = "ok"
    /\ file' = [file EXCEPT ![a] = held[a]] /\ held' = [held EXCEPT ![a] = None]
    /\ signedin' = [signedin EXCEPT ![a] = TRUE]
    /\ log' = {}
    /\ UNCHANGED <<next, flow, proc, procs, cbs, badcb, losses, up, disk, failures, taken, lost, restarts, said>>
    /\ UNCHANGED win

DiskFails ==
    /\ disk = "ok" /\ failures < MaxFailures
    /\ disk' = "failing" /\ failures' = failures + 1 /\ log' = {}
    /\ UNCHANGED <<file, held, taken, lost, restarts, said>> /\ UNCHANGED srv /\ UNCHANGED win /\ UNCHANGED up

DiskHeals ==
    /\ disk = "failing" /\ disk' = "ok" /\ log' = {}
    /\ UNCHANGED <<file, held, failures, taken, lost, restarts, said>> /\ UNCHANGED srv /\ UNCHANGED win /\ UNCHANGED up

\* The server ends and starts again: every live flow is dropped, every process ends with it, every held token is
\* lost. The flow counter is durable. Windows keep their stale views until they sync.
Restart ==
    /\ restarts < MaxRestarts
    /\ restarts' = restarts + 1
    /\ flow' = [i \in Ids |-> IF flow[i].st \in Live THEN [flow[i] EXCEPT !.st = "dropped", !.opener = None]
                              ELSE flow[i]]
    /\ proc' = [a \in Accounts |-> NoProc] /\ procs' = [a \in Accounts |-> 0]
    /\ held' = [a \in Accounts |-> None]
    /\ lost' = lost \cup {i \in Ids : taken[i] # None /\ held[flow[i].acct] = taken[i]}
    /\ log' = {[id |-> i, event |-> "dropped"] : i \in {j \in Ids : flow[j].st \in Live}}
    /\ UNCHANGED <<next, cbs, badcb, signedin, losses, up, file, disk, failures, taken, said>> /\ UNCHANGED win

----------------------------------------------------------------------------
\* The windows.

Quiet == log' = {}

\* The person shows the overlay (the hotkey): the window's next claim may go. It hides again.
Show(c) ==
    /\ conn[c] /\ ~armed[c] /\ armed' = [armed EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<conn, view, claimed, opened, browser, gate, up>> /\ UNCHANGED srv /\ UNCHANGED store /\ Quiet
Hide(c) ==
    /\ armed[c] /\ armed' = [armed EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<conn, view, claimed, opened, browser, gate, up>> /\ UNCHANGED srv /\ UNCHANGED store /\ Quiet

\* A window receives the server's state, its own view of it. Its gate closes when the flow it waits for is no longer one
\* the window holds (it ended, or the window was let go of it).
Sync(c) ==
    /\ conn[c] /\ view[c] # PubFor(c) /\ view' = [view EXCEPT ![c] = PubFor(c)]
    /\ gate' = IF gate[c] # 0 /\ ~\E p \in PubFor(c) : p.id = gate[c] /\ p.opener = c
               THEN [gate EXCEPT ![c] = 0] ELSE gate
    /\ UNCHANGED <<conn, armed, claimed, opened, browser, up>> /\ UNCHANGED srv /\ UNCHANGED store /\ Quiet

\* A shown window claims the oldest offer no window holds that it has not claimed before; the show is spent.
Claim(c) ==
    /\ armed[c]
    /\ LET free == {o.id : o \in {p \in view[c] : p.opener = None /\ p.id \notin claimed[c]}} IN
       /\ free # {}
       /\ up' = up \cup {[kind |-> "claim", id |-> Min(free), c |-> c, match |-> TRUE, ok |-> TRUE]}
       /\ claimed' = [claimed EXCEPT ![c] = @ \cup {Min(free)}]
    /\ armed' = [armed EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<conn, view, opened, browser, gate>> /\ UNCHANGED srv /\ UNCHANGED store /\ Quiet

\* The window holding an offer with its URL opens it in the system browser, once; on another machine than the server
\* it binds its one-shot gate for that flow first, and so waits, the page unopened, while its gate waits for another
\* flow's callback (one gate per window). F(c) is when a window may open a page.
OpenWith(F(_), c) ==
    /\ F(c)
    /\ \E o \in view[c] :
        /\ o.opener = c /\ o.url /\ o.id \notin opened[c]
        /\ opened' = [opened EXCEPT ![c] = @ \cup {o.id}]
        /\ browser' = [browser EXCEPT ![c] = @ \cup {o.id}]
        /\ gate' = IF c \in Remote THEN [gate EXCEPT ![c] = o.id] ELSE gate
        /\ up' = up \cup {[kind |-> "opened", id |-> o.id, c |-> c, match |-> TRUE, ok |-> TRUE]}
    /\ UNCHANGED <<conn, armed, view, claimed>> /\ UNCHANGED srv /\ UNCHANGED store /\ Quiet

Open(c) == OpenWith(LAMBDA x : x \notin Remote \/ gate[x] = 0, c)

\* The person signs in (ok) or declines on a page, and the browser is sent to the redirect. On the server's machine it
\* reaches the listener of the process (only the one serving that flow takes it: a newer one's state differs); on another
\* machine it reaches the window's gate, which sends its query up once, for the flow it waits for. G(c, i, ok) is what a
\* gate does with it.
GateCorrect(c, i, ok) ==
    IF gate[c] = i
    THEN /\ up' = up \cup {[kind |-> "callback", id |-> i, c |-> c, match |-> TRUE, ok |-> ok]}
         /\ gate' = [gate EXCEPT ![c] = 0]
    ELSE UNCHANGED <<up, gate>>

RedirectWith(G(_, _, _), c, i, ok) ==
    /\ i \in browser[c]
    /\ browser' = [browser EXCEPT ![c] = @ \ {i}]
    /\ IF c \in Remote
       THEN G(c, i, ok) /\ UNCHANGED proc
       ELSE /\ LET a == flow[i].acct IN
               proc' = IF proc[a].id = i /\ proc[a].st = "url"
                       THEN [proc EXCEPT ![a].st = IF ok THEN "got-code" ELSE "got-denied"]
                       ELSE proc
            /\ UNCHANGED <<up, gate>>
    /\ UNCHANGED <<conn, armed, view, claimed, opened>>
    /\ UNCHANGED <<next, flow, procs, cbs, badcb, signedin, losses>> /\ UNCHANGED store /\ Quiet

Redirect(c, i, ok) == RedirectWith(GateCorrect, c, i, ok)

\* A request on a window's gate that is not its callback (another path, another state, a second request): answered
\* "not found", never sent up. S(c) is what a gate does with it.
StrayWith(S(_), c) == gate[c] # 0 /\ S(c) /\ UNCHANGED <<conn, armed, view, claimed, opened, browser>> /\
                      UNCHANGED srv /\ UNCHANGED store /\ Quiet
StrayCorrect(c) == UNCHANGED <<up, gate>>
Stray(c) == StrayWith(StrayCorrect, c)

\* A window loses its link or quits: its gate closes. The server sees the link end and lets go of the flows the
\* window held, so that another window (or this one, back) can take them over; nothing starts for that.
Disconnect(c) ==
    /\ conn[c]
    /\ conn' = [conn EXCEPT ![c] = FALSE] /\ view' = [view EXCEPT ![c] = {}]
    /\ armed' = [armed EXCEPT ![c] = FALSE] /\ gate' = [gate EXCEPT ![c] = 0]
    /\ flow' = [i \in Ids |-> IF flow[i].opener = c THEN [flow[i] EXCEPT !.opener = None] ELSE flow[i]]
    /\ UNCHANGED <<claimed, opened, browser, up>> /\ UNCHANGED store /\ Quiet
    /\ UNCHANGED <<next, proc, procs, cbs, badcb, signedin, losses>>

Connect(c) ==
    /\ ~conn[c] /\ conn' = [conn EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<armed, view, claimed, opened, browser, gate, up>> /\ UNCHANGED srv /\ UNCHANGED store /\ Quiet

----------------------------------------------------------------------------

ServerNextWith(N(_), R(_), P(_, _), T(_)) ==
    \/ \E a \in Accounts : N(a) \/ Lose(a) \/ Url(a) \/ Exit(a) \/ End(a) \/ Retry(a)
    \/ \E a \in Accounts, v \in Tokens : P(a, v)
    \/ \E m \in up : R(m)
    \/ \E i \in Ids : T(i) \/ FallBack(i)
    \/ DiskFails \/ DiskHeals \/ Restart

WindowNextOpening(O(_), Rd(_, _, _), St(_)) ==
    \/ \E c \in Clients : Show(c) \/ Hide(c) \/ Sync(c) \/ Claim(c) \/ O(c) \/ St(c) \/ Disconnect(c) \/ Connect(c)
    \/ \E c \in Clients, i \in Ids, ok \in BOOLEAN : Rd(c, i, ok)

WindowNextWith(Rd(_, _, _), St(_)) == WindowNextOpening(Open, Rd, St)

Next == ServerNextWith(Need, Receive, Print, Timeout) \/ WindowNextWith(Redirect, Stray)

\* Deadlines pass, processes told to end do, messages arrive, windows receive the state, a failing disk is fixed and a
\* held token is written again. Nothing makes a person sign in.
Fairness ==
    /\ \A i \in Ids : WF_vars(Timeout(i))
    /\ \A a \in Accounts : WF_vars(End(a))
    /\ WF_vars(\E m \in up : Receive(m))
    /\ \A c \in Clients : WF_vars(Sync(c))
    /\ WF_vars(DiskHeals)
    /\ \A a \in Accounts : WF_vars(Retry(a))

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------
\* Properties.

NoValueIn(x) == \A f \in DOMAIN x : x[f] \notin Tokens

TypeOK ==
    /\ next \in 1 .. MaxFlows + 1
    /\ \A i \in Ids : flow[i].st \in States /\ flow[i].opener \in Clients \cup {None} /\ flow[i].denied \in BOOLEAN
    /\ \A a \in Accounts : proc[a].st \in ProcStates /\ proc[a].id \in 0 .. MaxFlows /\ proc[a].ending \in BOOLEAN
    /\ \A c \in Clients : gate[c] \in 0 .. MaxFlows /\ claimed[c] \subseteq Ids /\ opened[c] \subseteq Ids
    /\ file \in [Accounts -> Tokens \cup {None}] /\ held \in [Accounts -> Tokens \cup {None}]
    /\ taken \in [Ids -> Tokens \cup {None}] /\ said \in [Ids -> Tokens \cup {None}]
    /\ lost \subseteq Ids /\ badcb \subseteq Ids
    /\ disk \in {"ok", "failing"}

\* At most one flow is live for an account, and at most one `claude setup-token` runs for it.
OneFlowPerAccount == \A a \in Accounts : Cardinality(LiveOf(a)) <= 1
OneProcessPerAccount == \A a \in Accounts : procs[a] <= 1
\* A process runs for the flow it was started for: while that flow runs, or, told to end, after it ended; none is left
\* behind by a flow that ended without being told to end.
ProcessHasItsFlow ==
    \A a \in Accounts : proc[a].st # "none" =>
        /\ flow[proc[a].id].acct = a
        /\ (flow[proc[a].id].st \in Running) = ~proc[a].ending
\* Every token a process printed is taken, in time or late: none is dropped for its flow having ended first.
EveryPrintTaken == \A i \in Ids : said[i] # None => taken[i] = said[i]
\* Every page a window on another machine has open, for a flow it holds and waits on, has that window's gate bound for
\* it: a second flow's page never takes the gate from the first's.
GateForEveryPage ==
    \A c \in Remote : \A i \in browser[c] :
        (flow[i].opener = c /\ flow[i].st \in {"url-sent", "waiting"}) => gate[c] = i
\* Exactly one callback is replayed for a flow, and only one whose state matched the URL opened.
OneCallbackPerFlow == \A i \in Ids : cbs[i] <= 1
CallbackOnlyMatching == badcb = {}
\* A window's gate sends up only the callback whose state matches the URL it opened.
GateForwardsOnlyMatching == \A m \in up : m.kind = "callback" => m.match
\* The one-chance rule: the newest token captured for an account is in its file, or held and being written again, or
\* went with a restart (lost: the one documented window).
NewestTaken(i) == taken[i] # None /\ ~\E j \in Ids : j > i /\ flow[j].acct = flow[i].acct /\ taken[j] # None
TokenNeverDropped ==
    \A i \in Ids : NewestTaken(i) => file[flow[i].acct] = taken[i] \/ held[flow[i].acct] = taken[i] \/ i \in lost
\* A token is held only for a flow that took it (captured it, or took it late).
HeldOnlyCaptured ==
    \A a \in Accounts : held[a] # None => \E i \in Ids : flow[i].acct = a /\ taken[i] = held[a]
\* Signed in means a login is in the file.
SignedInHasLogin == \A a \in Accounts : signedin[a] => file[a] # None
\* No token reaches server.log or any window's state.
NoTokenInLog == \A x \in log : NoValueIn(x)
NoTokenToWindows == \A c \in Clients : \A p \in view[c] : NoValueIn(p)

\* A process starts only when a needed flow is claimed: a second window, or a window reconnected, mid-flow starts none.
ProcessOnlyFromNeeded ==
    [][\A a \in Accounts : procs'[a] > procs[a] =>
           \E i \in Ids : flow[i].acct = a /\ flow[i].st = "needed" /\ flow'[i].st = "started"]_vars
\* A flow that ended never changes again, and its id is never used again.
EndedStaysEnded ==
    [][\A i \in Ids : flow[i].st \in {"captured", "failed", "cancelled", "expired", "dropped"} => flow'[i] = flow[i]]_vars

\* Every running flow ends: captured, failed, cancelled, expired or dropped.
RunningEnds == \A i \in Ids : flow[i].st \in Running ~> flow[i].st \notin Running
\* Every held token is written in the end, unless the server ends first.
HeldIsWritten == \A a \in Accounts : held[a] # None ~> held[a] = None
\* Every process told to end ends.
ProcessEnds == \A a \in Accounts : proc[a].ending ~> proc[a].st = "none"
=============================================================================
