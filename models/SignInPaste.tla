----------------------------- MODULE SignInPaste -----------------------------
(***************************************************************************)
(* Sign-in by a pasted code (docs/accounts.md "Sign in"; src/sync/signin.h *)
(* SignInDesk::codeWanted and SignInDesk::code): how an account whose      *)
(* vendor ends its browser sign-in on a page saying "paste this code into  *)
(* your application" (Antigravity's Google login) is signed in from any    *)
(* nexus window, on the server's machine or on another one (an iPad, an   *)
(* iPhone, a Mac over ssh), with the code passing only from the window     *)
(* that started the flow to the one process waiting for it, once.          *)
(*                                                                         *)
(* One account. The server marks it needed (a flow, its id durable and    *)
(* never reused). A window claims it (the person pressed Sign in in that   *)
(* tab): the first claim of a needed flow starts the account's enrollment *)
(* process; a claim of a running flow no window holds takes it over; any  *)
(* other claim is late. The process hands over the vendor's sign-in URL    *)
(* (url-sent), which the server offers to the holding window alone, and    *)
(* which that window opens in its own browser: nothing binds a port, so   *)
(* where the window runs does not matter. Once the vendor's CLI asks for   *)
(* the code, the process says so (asked), and the holding window alone is  *)
(* told; the person pastes the code into that window, which sends it up    *)
(* once. The server passes a code to the waiting process only when the    *)
(* flow is running, asked, not yet sent a code, and held by the very       *)
(* window that sent it; every other code is refused with a plain line and  *)
(* never reaches a process (Stray: any window may send anything, for any   *)
(* id, at any time). The process then enrolls (the account is signed in)  *)
(* or exits (the flow fails: a wrong code is not retried, the person       *)
(* starts again). A deadline ends any running flow. A window whose link    *)
(* ends lets go of what it held; a server restart drops every flow.        *)
(*                                                                         *)
(* Remote windows act exactly as local ones: the reachability check        *)
(* MCSignInPasteRemote.cfg (one window, on another machine) must find the  *)
(* account signed in (NeverSignedIn violated), and the reversed witness    *)
(* witnesses/WSignInPasteLocalOnly.tla (the page on the server's loopback, *)
(* as before) must not.                                                    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Clients,     \* the windows
    Remote,      \* those on another machine than the server
    Codes,       \* the codes a vendor page may show: opaque
    MaxFlows,    \* flow ids 1..MaxFlows
    MaxStrays,   \* how many hostile or stale codes the windows send in a behaviour
    MaxRestarts  \* how many times the server restarts

ASSUME Remote \subseteq Clients /\ MaxFlows >= 1 /\ MaxStrays >= 0 /\ MaxRestarts >= 0

VARIABLES
    next,       \* the next flow id: durable
    flow,       \* each id's flow: state, opener (the window holding it), asked, sent
    proc,       \* the account's enrollment process: its state, the flow it serves, the code it was given
    conn,       \* which windows are linked
    view,       \* the offers each window last received
    claimed,    \* the offers each window has claimed
    opened,     \* the offers each window has opened in its browser
    pasted,     \* the flows each window has sent a code for from its dialog (never twice)
    up,         \* the windows' messages in flight, each delivered once
    strays,     \* how many stray codes were sent
    signedin,   \* the account is signed in
    restarts,
    sent,       \* ghost: how many codes reached each flow's process
    badfrom,    \* ghost: the flows a code reached from a window that did not hold the flow
    unasked,    \* ghost: the flows a code reached while their process was not asking
    log         \* the lines the last step wrote to server.log

vars == <<next, flow, proc, conn, view, claimed, opened, pasted, up, strays, signedin, restarts, sent, badfrom, unasked,
          log>>
win == <<conn, view, claimed, opened, pasted>>

None == "none"
Held == "held"
Ids == 1 .. MaxFlows
States == {"unused", "needed", "started", "url-sent", "waiting", "captured", "failed", "expired", "dropped"}
Live == {"needed", "started", "url-sent", "waiting"}
Running == {"started", "url-sent", "waiting"}
\* running: started, no URL yet; url: URL handed over; asking: the vendor's CLI waits for the code; got-code: it has one.
ProcStates == {"none", "running", "url", "asking", "got-code"}
NoProc == [st |-> "none", id |-> 0, code |-> None]
NoFlow == [st |-> "unused", opener |-> None, asked |-> FALSE, sent |-> FALSE]

\* What window c receives: each live flow, its holder (c, or held by a window it cannot name), whether its URL is there
\* and whether a code is wanted, both for the holder alone. Never a code.
PubFor(c) == {[id |-> i,
               opener |-> IF flow[i].opener = None THEN None ELSE IF flow[i].opener = c THEN c ELSE Held,
               url |-> (flow[i].st \in Running \ {"started"} /\ flow[i].opener = c),
               ask |-> (flow[i].st \in Running /\ flow[i].asked /\ ~flow[i].sent /\ flow[i].opener = c)] :
              i \in {j \in Ids : flow[j].st \in Live}}

Init ==
    /\ next = 1
    /\ flow = [i \in Ids |-> NoFlow]
    /\ proc = NoProc
    /\ conn = [c \in Clients |-> TRUE]
    /\ view = [c \in Clients |-> {}]
    /\ claimed = [c \in Clients |-> {}]
    /\ opened = [c \in Clients |-> {}]
    /\ pasted = [c \in Clients |-> {}]
    /\ up = {}
    /\ strays = 0
    /\ signedin = FALSE
    /\ restarts = 0
    /\ sent = [i \in Ids |-> 0]
    /\ badfrom = {}
    /\ unasked = {}
    /\ log = {}

LiveIds == {i \in Ids : flow[i].st \in Live}
Ghosts == <<sent, badfrom, unasked>>

----------------------------------------------------------------------------
\* The server.

Need ==
    /\ ~signedin /\ LiveIds = {} /\ next <= MaxFlows
    /\ flow' = [flow EXCEPT ![next] = [NoFlow EXCEPT !.st = "needed"]]
    /\ next' = next + 1
    /\ log' = {[id |-> next, event |-> "needed"]}
    /\ UNCHANGED <<proc, up, strays, signedin, restarts>> /\ UNCHANGED win /\ UNCHANGED Ghosts

ClaimCorrect(m) ==
    LET i == m.id IN
    IF flow[i].st = "needed"
    THEN /\ flow' = [flow EXCEPT ![i].st = "started", ![i].opener = m.c]
         /\ proc' = [st |-> "running", id |-> i, code |-> None]
         /\ log' = {[id |-> i, event |-> "started"]}
    ELSE IF flow[i].st \in Running /\ flow[i].opener = None
    THEN /\ flow' = [flow EXCEPT ![i].opener = m.c]
         /\ log' = {[id |-> i, event |-> "claimed"]}
         /\ UNCHANGED proc
    ELSE /\ log' = {[id |-> i, event |-> "late"]} /\ UNCHANGED <<flow, proc>>

OpenedCorrect(m) ==
    IF flow[m.id].st = "url-sent" /\ flow[m.id].opener = m.c
    THEN flow' = [flow EXCEPT ![m.id].st = "waiting"] /\ log' = {[id |-> m.id, event |-> "waiting"]}
    ELSE UNCHANGED flow /\ log' = {[id |-> m.id, event |-> "late"]}

\* Whether the server passes code message m on: the flow runs, its process asked, no code went yet, and the window
\* that sent it holds the flow (src/sync/signin.cpp SignInDesk::code).
TakesCorrect(m) ==
    LET i == m.id IN
    flow[i].st \in Running /\ flow[i].asked /\ ~flow[i].sent /\ flow[i].opener = m.c

\* A code message: passed to the waiting process once, or refused (a plain line to the window, in no effect the code).
\* T(m) is whether the server takes it; the ghosts record what a taken code broke, whatever T says.
CodeWith(T(_), m) ==
    LET i == m.id IN
    IF T(m)
    THEN /\ flow' = [flow EXCEPT ![i].sent = TRUE, ![i].asked = FALSE]
         /\ proc' = IF proc.id = i /\ proc.st = "asking" THEN [proc EXCEPT !.st = "got-code", !.code = m.v] ELSE proc
         /\ sent' = [sent EXCEPT ![i] = @ + 1]
         /\ badfrom' = IF flow[i].opener # m.c THEN badfrom \cup {i} ELSE badfrom
         /\ unasked' = IF ~(proc.id = i /\ proc.st = "asking") THEN unasked \cup {i} ELSE unasked
         /\ log' = {[id |-> i, event |-> "code sent"]}
    ELSE /\ log' = {[id |-> i, event |-> "code refused"]}
         /\ UNCHANGED <<flow, proc, sent, badfrom, unasked>>

ReceiveWith(T(_), m) ==
    /\ m \in up
    /\ up' = up \ {m}
    /\ CASE m.kind = "claim"  -> ClaimCorrect(m) /\ UNCHANGED Ghosts
         [] m.kind = "opened" -> OpenedCorrect(m) /\ UNCHANGED <<proc>> /\ UNCHANGED Ghosts
         [] m.kind = "code"   -> CodeWith(T, m)
    /\ UNCHANGED <<next, strays, signedin, restarts>> /\ UNCHANGED win

Receive(m) == ReceiveWith(TakesCorrect, m)

\* The process hands the vendor's URL over (the enrollment helper's socket).
Url ==
    /\ proc.st = "running" /\ flow[proc.id].st = "started"
    /\ proc' = [proc EXCEPT !.st = "url"]
    /\ flow' = [flow EXCEPT ![proc.id].st = "url-sent"]
    /\ log' = {[id |-> proc.id, event |-> "url"]}
    /\ UNCHANGED <<next, up, strays, signedin, restarts>> /\ UNCHANGED win /\ UNCHANGED Ghosts

\* The vendor's CLI asks for the code; the process says so (SignInDesk::codeWanted).
Ask ==
    /\ proc.st = "url" /\ flow[proc.id].st \in Running
    /\ proc' = [proc EXCEPT !.st = "asking"]
    /\ flow' = [flow EXCEPT ![proc.id].asked = TRUE]
    /\ log' = {[id |-> proc.id, event |-> "code asked"]}
    /\ UNCHANGED <<next, up, strays, signedin, restarts>> /\ UNCHANGED win /\ UNCHANGED Ghosts

\* The vendor took the code: the enrollment is saved and the account signed in.
Enroll ==
    /\ proc.st = "got-code" /\ flow[proc.id].st \in Running
    /\ flow' = [flow EXCEPT ![proc.id].st = "captured", ![proc.id].opener = None]
    /\ signedin' = TRUE
    /\ proc' = NoProc
    /\ log' = {[id |-> proc.id, event |-> "captured"]}
    /\ UNCHANGED <<next, up, strays, restarts>> /\ UNCHANGED win /\ UNCHANGED Ghosts

\* The process ends with no enrollment (a wrong code, the person declined, the vendor failed): the flow fails.
Exit ==
    /\ proc.st # "none"
    /\ flow' = [flow EXCEPT ![proc.id].st = "failed", ![proc.id].opener = None]
    /\ proc' = NoProc
    /\ log' = {[id |-> proc.id, event |-> "failed"]}
    /\ UNCHANGED <<next, up, strays, signedin, restarts>> /\ UNCHANGED win /\ UNCHANGED Ghosts

\* A running flow's deadline passes: it expires and its process is ended.
Timeout(i) ==
    /\ flow[i].st \in Running
    /\ flow' = [flow EXCEPT ![i].st = "expired", ![i].opener = None]
    /\ proc' = IF proc.id = i THEN NoProc ELSE proc
    /\ log' = {[id |-> i, event |-> "expired"]}
    /\ UNCHANGED <<next, up, strays, signedin, restarts>> /\ UNCHANGED win /\ UNCHANGED Ghosts

Restart ==
    /\ restarts < MaxRestarts
    /\ restarts' = restarts + 1
    /\ flow' = [i \in Ids |-> IF flow[i].st \in Live THEN [flow[i] EXCEPT !.st = "dropped", !.opener = None]
                              ELSE flow[i]]
    /\ proc' = NoProc
    /\ log' = {[id |-> i, event |-> "dropped"] : i \in LiveIds}
    /\ UNCHANGED <<next, up, strays, signedin>> /\ UNCHANGED win /\ UNCHANGED Ghosts

----------------------------------------------------------------------------
\* The windows.

Quiet == log' = {}

Sync(c) ==
    /\ conn[c] /\ view[c] # PubFor(c) /\ view' = [view EXCEPT ![c] = PubFor(c)]
    /\ UNCHANGED <<conn, claimed, opened, pasted, up, strays>> /\ UNCHANGED <<next, flow, proc, signedin, restarts>>
    /\ UNCHANGED Ghosts /\ Quiet

\* The person presses Sign in in the tab: the window claims the flow no window holds, once. W(c) is whether window c
\* may claim (any window, here; only one on the server's machine in the old design, the witness's).
ClaimWith(W(_), c) ==
    /\ conn[c] /\ W(c)
    /\ \E o \in view[c] :
        /\ o.opener = None /\ o.id \notin claimed[c]
        /\ up' = up \cup {[kind |-> "claim", id |-> o.id, c |-> c, v |-> None]}
        /\ claimed' = [claimed EXCEPT ![c] = @ \cup {o.id}]
    /\ UNCHANGED <<conn, view, opened, pasted, strays>> /\ UNCHANGED <<next, flow, proc, signedin, restarts>>
    /\ UNCHANGED Ghosts /\ Quiet

Claim(c) == ClaimWith(LAMBDA x : TRUE, c)

\* The window holding an offer with its URL opens it in its own browser, once: no gate, wherever the window is.
Open(c) ==
    /\ conn[c]
    /\ \E o \in view[c] :
        /\ o.opener = c /\ o.url /\ o.id \notin opened[c]
        /\ opened' = [opened EXCEPT ![c] = @ \cup {o.id}]
        /\ up' = up \cup {[kind |-> "opened", id |-> o.id, c |-> c, v |-> None]}
    /\ UNCHANGED <<conn, view, claimed, pasted, strays>> /\ UNCHANGED <<next, flow, proc, signedin, restarts>>
    /\ UNCHANGED Ghosts /\ Quiet

\* The person pastes the code the vendor's page showed into the dialog of the window that holds the flow, whose view
\* says a code is wanted; Enter sends it once and the dialog closes, wiped.
Paste(c, v) ==
    /\ conn[c]
    /\ \E o \in view[c] :
        /\ o.opener = c /\ o.ask /\ o.id \in opened[c] /\ o.id \notin pasted[c]
        /\ pasted' = [pasted EXCEPT ![c] = @ \cup {o.id}]
        /\ up' = up \cup {[kind |-> "code", id |-> o.id, c |-> c, v |-> v]}
    /\ UNCHANGED <<conn, view, claimed, opened, strays>> /\ UNCHANGED <<next, flow, proc, signedin, restarts>>
    /\ UNCHANGED Ghosts /\ Quiet

\* Anything else a window may send: a code for any flow, held by it or not, asked or not, once more or not (a resent
\* action, a stale view, a hostile peer on the link).
Stray(c, i, v) ==
    /\ conn[c] /\ strays < MaxStrays
    /\ strays' = strays + 1
    /\ up' = up \cup {[kind |-> "code", id |-> i, c |-> c, v |-> v]}
    /\ UNCHANGED <<conn, view, claimed, opened, pasted>> /\ UNCHANGED <<next, flow, proc, signedin, restarts>>
    /\ UNCHANGED Ghosts /\ Quiet

\* A window's link ends: the server lets go of the flows it held.
Disconnect(c) ==
    /\ conn[c]
    /\ conn' = [conn EXCEPT ![c] = FALSE] /\ view' = [view EXCEPT ![c] = {}]
    /\ flow' = [i \in Ids |-> IF flow[i].opener = c THEN [flow[i] EXCEPT !.opener = None] ELSE flow[i]]
    /\ UNCHANGED <<claimed, opened, pasted, up, strays, next, proc, signedin, restarts>> /\ UNCHANGED Ghosts /\ Quiet

Connect(c) ==
    /\ ~conn[c] /\ conn' = [conn EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<view, claimed, opened, pasted, up, strays, next, flow, proc, signedin, restarts>>
    /\ UNCHANGED Ghosts /\ Quiet

----------------------------------------------------------------------------

ServerNextWith(T(_)) ==
    \/ Need \/ Url \/ Ask \/ Enroll \/ Exit \/ Restart
    \/ \E m \in up : ReceiveWith(T, m)
    \/ \E i \in Ids : Timeout(i)

WindowNextWith(W(_)) ==
    \/ \E c \in Clients : Sync(c) \/ ClaimWith(W, c) \/ Open(c) \/ Disconnect(c) \/ Connect(c)
    \/ \E c \in Clients, v \in Codes : Paste(c, v)
    \/ \E c \in Clients, i \in Ids, v \in Codes : Stray(c, i, v)

Next == ServerNextWith(TakesCorrect) \/ WindowNextWith(LAMBDA c : TRUE)

Fairness ==
    /\ \A i \in Ids : WF_vars(Timeout(i))
    /\ WF_vars(\E m \in up : Receive(m))
    /\ \A c \in Clients : WF_vars(Sync(c))

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------
\* Properties.

TypeOK ==
    /\ next \in 1 .. MaxFlows + 1
    /\ \A i \in Ids : flow[i].st \in States /\ flow[i].opener \in Clients \cup {None} /\
                      flow[i].asked \in BOOLEAN /\ flow[i].sent \in BOOLEAN
    /\ proc.st \in ProcStates /\ proc.id \in 0 .. MaxFlows /\ proc.code \in Codes \cup {None}
    /\ \A c \in Clients : claimed[c] \subseteq Ids /\ opened[c] \subseteq Ids /\ pasted[c] \subseteq Ids
    /\ strays \in 0 .. MaxStrays
    /\ signedin \in BOOLEAN
    /\ badfrom \subseteq Ids /\ unasked \subseteq Ids

\* At most one live flow.
OneFlow == Cardinality(LiveIds) <= 1
\* A process serves a running flow.
ProcessHasItsFlow == proc.st # "none" => flow[proc.id].st \in Running
\* One code, at most, ever reaches a flow's process: a second is refused.
OneCodePerFlow == \A i \in Ids : sent[i] <= 1
\* A code reaches a flow's process only from the window holding that flow: never from another window.
CodeOnlyFromHolder == badfrom = {}
\* A code reaches a flow's process only while that process waits for one: never with no flow waiting.
CodeOnlyWhenAsked == unasked = {}
\* No code in server.log, and none in any window's state (the dialog's field and the action up are the window's own).
NoCodeIn(r) == \A f \in DOMAIN r : r[f] \notin Codes
NoCodeInLog == \A r \in log : NoCodeIn(r)
NoCodeToWindows == \A c \in Clients : \A o \in view[c] : NoCodeIn(o)
\* Only a window holding the flow is ever told a code is wanted.
AskOnlyToHolder == \A c \in Clients : \A o \in view[c] : o.ask => o.opener = c

\* Reachability: with these windows, the account is never signed in. A check that must FAIL for the design to work
\* from them (MCSignInPasteRemote.cfg), and must pass for a design that keeps them out (WSignInPasteLocalOnly).
NeverSignedIn == ~signedin

EndedStaysEnded ==
    [][\A i \in Ids : flow[i].st \in {"captured", "failed", "expired", "dropped"} => flow'[i] = flow[i]]_vars
RunningEnds == \A i \in Ids : flow[i].st \in Running ~> flow[i].st \notin Running
=============================================================================
