------------------------------ MODULE Secrets ------------------------------
(***************************************************************************)
(* The secret dialog (docs/secrets.md): how a secret for an account gets  *)
(* from a person's fingers to its 0600 file on nexus-server, with no AI    *)
(* ever seeing it.                                                         *)
(*                                                                         *)
(* An asker (a friend's AI, through its driver, or the server itself, for  *)
(* a sign-in) asks the server for a secret for an account. The server      *)
(* mints a request id, never reused across restarts, and the request goes  *)
(* into the state every connected window receives: its id, its account    *)
(* and who asks, never a value. The server knows no tabs (AGENTS.md), so   *)
(* every window that can take a request is offered it. Each such window    *)
(* shows a dialog for the oldest open request it has not answered. A      *)
(* person pastes the value once into one window and submits it (or        *)
(* presses Escape, which cancels the request for every window). The value  *)
(* travels up that window's actions to the server alone. The server takes  *)
(* the first answer for an open request: it writes the value to the       *)
(* account's file and answers the asker "stored"; a cancel, a deadline or  *)
(* a refusal at the ask answers it "refused" with a reason. The other      *)
(* windows drop their dialogs when their state no longer holds the         *)
(* request. An answer for a request that is no longer open is dropped and  *)
(* logged without its value.                                               *)
(*                                                                         *)
(* A second request for an account that has one open is refused ("busy"). *)
(* A request asked when no connected window can take one (none connected, *)
(* or only windows of a version before secret requests) is refused         *)
(* ("no-client"). A server restart drops every open request with no       *)
(* answer: its asker is gone with it (a driver ends with the server, and   *)
(* the server's own sign-in asks again). A window that loses its link or   *)
(* quits loses its dialog and whatever was typed in it; the request stays  *)
(* open for the other windows, or until its deadline.                      *)
(*                                                                         *)
(* A secret is an opaque token, one of Values. The places an asker or an   *)
(* AI can see are replies (what each asker is answered), transcript (what  *)
(* shows in the asker's session) and log (server.log); view is what each   *)
(* window received in its state. No value may ever reach any of them: the  *)
(* only places one lives are a window's open dialog, the window's answer   *)
(* in flight (up), and the account's file (file).                          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Clients,     \* the windows that may connect
    OldClients,  \* those of a version before secret requests: never offered one
    Accounts,    \* the accounts a secret can be for
    Askers,      \* who may ask (a friend's AI through its driver, the server's own sign-in)
    Values,      \* the secrets a person may type: opaque tokens
    MaxRequests, \* how many requests are asked in a behaviour, ids 1..MaxRequests
    MaxRestarts  \* how many times the server restarts in a behaviour

ASSUME OldClients \subseteq Clients /\ MaxRequests >= 1 /\ MaxRestarts >= 0

VARIABLES
    next,       \* the next request id: durable, so an id is never reused across restarts
    req,        \* each id's request: its state, account and asker
    conn,       \* which windows have a link to the server
    view,       \* the open requests each window last received in its state
    dialog,     \* each window's dialog: the request it shows (0: none) and what was typed in it
    done,       \* the requests each window has answered or cancelled, so that it never shows them again
    up,         \* answers in flight from the windows to the server, each delivered once
    file,       \* each account's 0600 secret file
    stores,     \* ghost: how many times each request's value was written to its file
    replies,    \* what the askers have been answered
    transcript, \* the lines the last step wrote into the asker's session (what its AI reads back)
    log,        \* the lines the last step wrote to server.log
    restarts    \* how many times the server restarted

\* transcript and log hold only the last step's lines, so that a behaviour's whole session and log are the union over
\* its steps: an invariant on them checks every line ever written, without the states growing with the history.
vars == <<next, req, conn, view, dialog, done, up, file, stores, replies, transcript, log, restarts>>

\* A step that writes no line.
Quiet == transcript' = {} /\ log' = {}

Ids == 1 .. MaxRequests
NoValue == "none"
NoDialog == [id |-> 0, typed |-> NoValue]
States == {"unused", "open", "stored", "refused", "dropped"}  \* dropped: open at a restart, never answered
Reasons == {"none", "busy", "no-client", "cancelled", "timeout"}
Min(S) == CHOOSE x \in S : \A y \in S : x <= y

Capable(c) == c \notin OldClients
Takers == {c \in Clients : conn[c] /\ Capable(c)}
OpenIds == {r \in Ids : req[r].st = "open"}
\* What every window receives about the open requests: never a value.
Pub == {[id |-> r, acct |-> req[r].acct, asker |-> req[r].asker] : r \in OpenIds}

Init ==
    /\ next = 1
    /\ req = [r \in Ids |-> [st |-> "unused", acct |-> "none", asker |-> "none"]]
    /\ conn = [c \in Clients |-> TRUE]
    /\ view = [c \in Clients |-> {}]
    /\ dialog = [c \in Clients |-> NoDialog]
    /\ done = [c \in Clients |-> {}]
    /\ up = {}
    /\ file = [a \in Accounts |-> NoValue]
    /\ stores = [r \in Ids |-> 0]
    /\ replies = {}
    /\ transcript = {}
    /\ log = {}
    /\ restarts = 0

----------------------------------------------------------------------------
\* The parts of a step.

Reply(r, out, why) == [id |-> r, asker |-> req[r].asker, outcome |-> out, reason |-> why]

\* Close open request r as refused, for reason why: the asker is answered, its session and the log say so.
RefuseOpen(r, why) ==
    /\ req' = [req EXCEPT ![r].st = "refused"]
    /\ replies' = replies \cup {Reply(r, "refused", why)}
    /\ transcript' = {[id |-> r, what |-> "refused", acct |-> req[r].acct]}
    /\ log' = {[id |-> r, event |-> why]}

\* An asker asks for a secret for an account. Refused at once if one is open for the account (Busy), or no window can
\* take it (NoTaker).
Busy(acct) == \E q \in OpenIds : req[q].acct = acct
NoTaker == Takers = {}

AskWith(B(_), N, a, acct) ==
    /\ next <= MaxRequests
    /\ next' = next + 1
    /\ LET r == next
           asked == [st |-> "open", acct |-> acct, asker |-> a]
           refuse(why) ==
               /\ req' = [req EXCEPT ![r] = [asked EXCEPT !.st = "refused"]]
               /\ replies' = replies \cup {[id |-> r, asker |-> a, outcome |-> "refused", reason |-> why]}
               /\ transcript' = {[id |-> r, what |-> "refused", acct |-> acct]}
               /\ log' = {[id |-> r, event |-> why]}
       IN IF B(acct) THEN refuse("busy")
          ELSE IF N THEN refuse("no-client")
          ELSE /\ req' = [req EXCEPT ![r] = asked]
               /\ transcript' = {[id |-> r, what |-> "asked", acct |-> acct]}
               /\ log' = {[id |-> r, event |-> "asked"]}
               /\ UNCHANGED replies
    /\ UNCHANGED <<conn, view, dialog, done, up, file, stores, restarts>>

Ask(a, acct) == AskWith(Busy, NoTaker, a, acct)

\* A window receives the server's current state: its view becomes Pub. Its dialog stays while its request is in the new
\* view; otherwise it closes, and the oldest request in the view it has not answered opens in its place.
\* V(c) is what the window receives (Pub); D(c, ids) the dialog it shows after, given the ids it received.
SyncWith(V(_), D(_, _), c) ==
    /\ conn[c] /\ Capable(c)
    /\ LET v == V(c)
           d == D(c, {p.id : p \in v})
       IN /\ <<v, d>> # <<view[c], dialog[c]>>
          /\ view' = [view EXCEPT ![c] = v]
          /\ dialog' = [dialog EXCEPT ![c] = d]
    /\ UNCHANGED <<next, req, conn, done, up, file, stores, replies, restarts>> /\ Quiet

Received(c) == Pub
DialogAfter(c, ids) ==
    IF dialog[c].id \in ids THEN dialog[c]
    ELSE IF ids \ done[c] # {} THEN [id |-> Min(ids \ done[c]), typed |-> NoValue]
    ELSE NoDialog

Sync(c) == SyncWith(Received, DialogAfter, c)

\* The person pastes a value into the dialog: once; nothing echoes it.
Type(c, v) ==
    /\ dialog[c].id # 0 /\ dialog[c].typed = NoValue
    /\ dialog' = [dialog EXCEPT ![c].typed = v]
    /\ UNCHANGED <<next, req, conn, view, done, up, file, stores, replies, restarts>> /\ Quiet

\* Enter: the value goes up to the server, once; the window forgets it and closes the dialog.
Submit(c) ==
    /\ dialog[c].typed # NoValue
    /\ up' = up \cup {[id |-> dialog[c].id, kind |-> "secret", v |-> dialog[c].typed]}
    /\ done' = [done EXCEPT ![c] = @ \cup {dialog[c].id}]
    /\ dialog' = [dialog EXCEPT ![c] = NoDialog]
    /\ UNCHANGED <<next, req, conn, view, file, stores, replies, restarts>> /\ Quiet

\* Escape: the request is cancelled, for every window.
Cancel(c) ==
    /\ dialog[c].id # 0
    /\ up' = up \cup {[id |-> dialog[c].id, kind |-> "cancel", v |-> NoValue]}
    /\ done' = [done EXCEPT ![c] = @ \cup {dialog[c].id}]
    /\ dialog' = [dialog EXCEPT ![c] = NoDialog]
    /\ UNCHANGED <<next, req, conn, view, file, stores, replies, restarts>> /\ Quiet

\* A window loses its link or quits: its dialog and what was typed in it are gone. What it already sent stays in flight.
Disconnect(c) ==
    /\ conn[c]
    /\ conn' = [conn EXCEPT ![c] = FALSE]
    /\ view' = [view EXCEPT ![c] = {}]
    /\ dialog' = [dialog EXCEPT ![c] = NoDialog]
    /\ done' = [done EXCEPT ![c] = {}]
    /\ UNCHANGED <<next, req, up, file, stores, replies, restarts>> /\ Quiet

Connect(c) ==
    /\ ~conn[c]
    /\ conn' = [conn EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<next, req, view, dialog, done, up, file, stores, replies, restarts>> /\ Quiet

\* The server applies an answer, against the request's state when it processes it. Only an open request takes one.
\* Late(m) is what it does with an answer for a request that is not open.
StoreOpen(m) ==
    LET r == m.id IN
    /\ req' = [req EXCEPT ![r].st = "stored"]
    /\ file' = [file EXCEPT ![req[r].acct] = m.v]
    /\ stores' = [stores EXCEPT ![r] = @ + 1]
    /\ replies' = replies \cup {Reply(r, "stored", "none")}
    /\ transcript' = {[id |-> r, what |-> "stored", acct |-> req[r].acct]}
    /\ log' = {[id |-> r, event |-> "stored"]}

DropLate(m) ==
    /\ log' = {[id |-> m.id, event |-> "late"]}
    /\ UNCHANGED <<req, file, stores, replies>> /\ transcript' = {}

ReceiveWith(St(_), Late(_), m) ==
    /\ m \in up
    /\ up' = up \ {m}
    /\ IF req[m.id].st = "open" /\ m.kind = "secret" THEN St(m)
       ELSE IF req[m.id].st = "open" /\ m.kind = "cancel" THEN RefuseOpen(m.id, "cancelled") /\ UNCHANGED <<file, stores>>
       ELSE Late(m)
    /\ UNCHANGED <<next, conn, view, dialog, done, restarts>>

Receive(m) == ReceiveWith(StoreOpen, DropLate, m)

\* The request's deadline passes with no answer.
Timeout(r) ==
    /\ req[r].st = "open"
    /\ RefuseOpen(r, "timeout")
    /\ UNCHANGED <<next, conn, view, dialog, done, up, file, stores, restarts>>

\* The server ends (a crash or a stop) and starts again. Requests are not durable: each open one is dropped, unanswered,
\* its asker gone with the old server. The id counter is durable. Windows keep their stale views until they sync.
RestartWith(N(_)) ==
    /\ restarts < MaxRestarts
    /\ restarts' = restarts + 1
    /\ req' = [r \in Ids |-> IF req[r].st = "open" THEN [req[r] EXCEPT !.st = "dropped"] ELSE req[r]]
    /\ next' = N(next)
    /\ log' = {[id |-> r, event |-> "dropped"] : r \in OpenIds}
    /\ UNCHANGED <<conn, view, dialog, done, up, file, stores, replies>> /\ transcript' = {}

Restart == RestartWith(LAMBDA n : n)

NextWith(A(_, _), S(_), R(_), T(_), Rs) ==
    \/ \E a \in Askers, acct \in Accounts : A(a, acct)
    \/ \E c \in Clients : S(c) \/ Submit(c) \/ Cancel(c) \/ Disconnect(c) \/ Connect(c)
    \/ \E c \in Clients, v \in Values : Type(c, v)
    \/ \E m \in up : R(m)
    \/ \E r \in Ids : T(r)
    \/ Rs

Next == NextWith(Ask, Sync, Receive, Timeout, Restart)

\* Deadlines pass, windows receive the state, and answers in flight arrive. Nothing makes a person answer.
Fairness ==
    /\ \A r \in Ids : WF_vars(Timeout(r))
    /\ \A c \in Clients : WF_vars(Sync(c))
    /\ WF_vars(\E m \in up : Receive(m))

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------
\* Properties.

NoValueIn(x) == \A f \in DOMAIN x : x[f] \notin Values

TypeOK ==
    /\ next \in 1 .. MaxRequests + 1
    /\ \A r \in Ids : req[r].st \in States
    /\ conn \in [Clients -> BOOLEAN]
    /\ \A c \in Clients : dialog[c].id \in 0 .. MaxRequests /\ dialog[c].typed \in Values \cup {NoValue}
    /\ \A c \in Clients : done[c] \subseteq Ids
    /\ \A m \in up : m.kind \in {"secret", "cancel"} /\ (m.v \in Values <=> m.kind = "secret")
    /\ file \in [Accounts -> Values \cup {NoValue}]
    /\ \A x \in replies : x.outcome \in {"stored", "refused"} /\ x.reason \in Reasons

\* The secret never reaches the asker: an asker hears "stored" or "refused" and a reason, nothing else.
NoSecretToAsker == \A x \in replies : NoValueIn(x) /\ x.outcome \in {"stored", "refused"}
\* Nor the asker's session (what its AI reads back), nor the server's log.
NoSecretInTranscript == \A x \in transcript : NoValueIn(x)
NoSecretInLog == \A x \in log : NoValueIn(x)
\* The answer goes to the server alone: no window ever receives a value in its state, its own or another's.
NoSecretToWindows == \A c \in Clients : \A p \in view[c] : NoValueIn(p)

\* At most one value is stored per request, and one is stored exactly when the request ended stored.
AtMostOneStored == \A r \in Ids : stores[r] <= 1
StoredWhenStored == \A r \in Ids : stores[r] = 1 <=> req[r].st = "stored"
\* At most one request is open per account.
OneOpenPerAccount == \A q, r \in OpenIds : req[q].acct = req[r].acct => q = r
\* Each asker is answered at most once per request, and exactly when its request ended stored or refused (a request
\* dropped at a restart has no asker left to answer).
AnsweredOnce == \A r \in Ids : Cardinality({x \in replies : x.id = r}) <= 1
AnsweredWhenEnded == \A r \in Ids : (req[r].st \in {"stored", "refused"}) <=> (\E x \in replies : x.id = r)

\* A closed request is never satisfied later, nor reopened, nor its id reused: once it has ended, it never changes.
ClosedStaysClosed ==
    [][\A r \in Ids : req[r].st \in {"stored", "refused", "dropped"} =>
                          req'[r] = req[r] /\ stores'[r] = stores[r]]_vars

\* A request opens only while some connected window can take it; otherwise its asker is told "no-client" at once.
AskNeedsTaker == [][\A r \in Ids : req[r].st = "unused" /\ req'[r].st = "open" => Takers # {}]_vars

\* Every request ends: stored, refused or dropped.
EveryRequestEnds == \A r \in Ids : req[r].st = "open" ~> req[r].st # "open"
\* Every window's dialog for a request that has ended closes.
DialogsClose == \A c \in Clients, r \in Ids : (dialog[c].id = r /\ req[r].st # "open") ~> dialog[c].id # r
=============================================================================
