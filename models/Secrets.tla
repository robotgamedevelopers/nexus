------------------------------ MODULE Secrets ------------------------------
(***************************************************************************)
(* The secret dialog (docs/secrets.md): how a secret for an account gets  *)
(* from a person's fingers to its 0600 file on nexus-server, with no AI    *)
(* ever seeing it, and never pasted twice for a failure of nexus's own.    *)
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
(* the first answer for an open request. A value not of the file's shape   *)
(* (BadValues: empty, two lines, a sentence) is the person's input and is  *)
(* refused, "malformed", so that they are asked again. A value of the      *)
(* right shape is written to the account's file: when the disk takes it,   *)
(* the asker hears "stored"; when it does not (the disk full, the          *)
(* directory unwritable, its sync failed), the server holds the value in   *)
(* memory (held) and writes it again until the disk takes it, and only     *)
(* then answers "stored". A cancel, a deadline or a refusal at the ask     *)
(* answers "refused" with a reason. A request past its deadline expires:   *)
(* an answer that crossed the deadline is still stored, while no newer     *)
(* request for its account has been open. Other windows drop their        *)
(* dialogs when their state no longer holds the request. Any other answer  *)
(* for a request that is no longer open is dropped and logged without its  *)
(* value.                                                                  *)
(*                                                                         *)
(* A second request for an account that has one open or held is refused   *)
(* ("busy"). A request asked when no connected window can take one (none   *)
(* connected, or only windows of a version before secret requests) is      *)
(* refused ("no-client"). A server restart drops every open request with   *)
(* no answer (its asker is gone with it), and loses every held value: the  *)
(* one way a value that arrived is not stored (lost, and docs/secrets.md   *)
(* says so). A window that loses its link or quits loses its dialog and    *)
(* whatever was typed in it; the request stays open for the other windows, *)
(* or until its deadline.                                                  *)
(*                                                                         *)
(* A secret is an opaque token, one of Values. The places an asker or an   *)
(* AI can see are replies (what each asker is answered), transcript (what  *)
(* shows in the asker's session) and log (server.log); view is what each   *)
(* window received in its state. No value may ever reach any of them: the  *)
(* only places one lives are a window's open dialog, the window's answer   *)
(* in flight (up), the server's memory while its write is retried (held),  *)
(* and the account's file (file).                                          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Clients,     \* the windows that may connect
    OldClients,  \* those of a version before secret requests: never offered one
    Accounts,    \* the accounts a secret can be for
    Askers,      \* who may ask (a friend's AI through its driver, the server's own sign-in)
    Values,      \* the secrets a person may type: opaque tokens
    BadValues,   \* those not of the file's shape (secretShape): the person's input, refused "malformed"
    MaxRequests, \* how many requests are asked in a behaviour, ids 1..MaxRequests
    MaxRestarts, \* how many times the server restarts in a behaviour
    MaxFailures  \* how many times the disk starts failing in a behaviour

ASSUME OldClients \subseteq Clients /\ BadValues \subseteq Values /\ MaxRequests >= 1 /\ MaxRestarts >= 0 /\
       MaxFailures >= 0

VARIABLES
    next,       \* the next request id: durable, so an id is never reused across restarts
    req,        \* each id's request: its state, account and asker
    conn,       \* which windows have a link to the server
    view,       \* the open requests each window last received in its state
    dialog,     \* each window's dialog: the request it shows (0: none) and what was typed in it
    done,       \* the requests each window has answered or cancelled, so that it never shows them again
    up,         \* answers in flight from the windows to the server, each delivered once
    file,       \* each account's 0600 secret file
    stores,     \* ghost: how many times each request's value was written to its file durably
    replies,    \* what the askers have been answered
    transcript, \* the lines the last step wrote into the asker's session (what its AI reads back)
    log,        \* the lines the last step wrote to server.log
    restarts,   \* how many times the server restarted
    disk,       \* whether a write of an account's file is durable now ("ok") or fails ("failing")
    failures,   \* how many times the disk started failing
    held,       \* each request's value the server holds in memory while its write is retried
    taken,      \* ghost: the value the server received for each request and must not drop
    lost        \* ghost: the requests whose held value went with a restart

\* transcript and log hold only the last step's lines, so that a behaviour's whole session and log are the union over
\* its steps: an invariant on them checks every line ever written, without the states growing with the history.
vars == <<next, req, conn, view, dialog, done, up, file, stores, replies, transcript, log, restarts, disk, failures,
          held, taken, lost>>
env == <<disk, failures>>
mem == <<held, taken, lost>>

\* A step that writes no line.
Quiet == transcript' = {} /\ log' = {}

Ids == 1 .. MaxRequests
NoValue == "none"
NoDialog == [id |-> 0, typed |-> NoValue]
\* holding: the value arrived and is held until its write is durable; expired: refused (timeout), its late answer still
\* taken while it is the newest of its account; dropped: open or holding at a restart.
States == {"unused", "open", "holding", "stored", "refused", "expired", "dropped"}
Reasons == {"none", "busy", "no-client", "cancelled", "timeout", "malformed"}
Min(S) == CHOOSE x \in S : \A y \in S : x <= y

Capable(c) == c \notin OldClients
Takers == {c \in Clients : conn[c] /\ Capable(c)}
OpenIds == {r \in Ids : req[r].st = "open"}
HoldingIds == {r \in Ids : req[r].st = "holding"}
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
    /\ disk = "ok"
    /\ failures = 0
    /\ held = [r \in Ids |-> NoValue]
    /\ taken = [r \in Ids |-> NoValue]
    /\ lost = {}

----------------------------------------------------------------------------
\* The parts of a step.

Reply(r, out, why) == [id |-> r, asker |-> req[r].asker, outcome |-> out, reason |-> why]
Replied(r) == \E x \in replies : x.id = r

\* Close open request r into state st, refused for reason why: the asker is answered, its session and the log say so.
RefuseAs(r, st, why) ==
    /\ req' = [req EXCEPT ![r].st = st]
    /\ replies' = replies \cup {Reply(r, "refused", why)}
    /\ transcript' = {[id |-> r, what |-> "refused", acct |-> req[r].acct]}
    /\ log' = {[id |-> r, event |-> why]}

RefuseOpen(r, why) == RefuseAs(r, "refused", why)

\* An asker asks for a secret for an account. Refused at once if one is open or held for the account (Busy), or no
\* window can take it (NoTaker).
Busy(acct) == \E q \in OpenIds \cup HoldingIds : req[q].acct = acct
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
    /\ UNCHANGED <<conn, view, dialog, done, up, file, stores, restarts>> /\ UNCHANGED env /\ UNCHANGED mem

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
    /\ UNCHANGED <<next, req, conn, done, up, file, stores, replies, restarts>> /\ UNCHANGED env /\ UNCHANGED mem
    /\ Quiet

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
    /\ UNCHANGED <<next, req, conn, view, done, up, file, stores, replies, restarts>> /\ UNCHANGED env
    /\ UNCHANGED mem /\ Quiet

\* Enter: the value goes up to the server, once; the window forgets it and closes the dialog.
Submit(c) ==
    /\ dialog[c].typed # NoValue
    /\ up' = up \cup {[id |-> dialog[c].id, kind |-> "secret", v |-> dialog[c].typed]}
    /\ done' = [done EXCEPT ![c] = @ \cup {dialog[c].id}]
    /\ dialog' = [dialog EXCEPT ![c] = NoDialog]
    /\ UNCHANGED <<next, req, conn, view, file, stores, replies, restarts>> /\ UNCHANGED env /\ UNCHANGED mem
    /\ Quiet

\* Escape: the request is cancelled, for every window.
Cancel(c) ==
    /\ dialog[c].id # 0
    /\ up' = up \cup {[id |-> dialog[c].id, kind |-> "cancel", v |-> NoValue]}
    /\ done' = [done EXCEPT ![c] = @ \cup {dialog[c].id}]
    /\ dialog' = [dialog EXCEPT ![c] = NoDialog]
    /\ UNCHANGED <<next, req, conn, view, file, stores, replies, restarts>> /\ UNCHANGED env /\ UNCHANGED mem
    /\ Quiet

\* A window loses its link or quits: its dialog and what was typed in it are gone. What it already sent stays in flight.
Disconnect(c) ==
    /\ conn[c]
    /\ conn' = [conn EXCEPT ![c] = FALSE]
    /\ view' = [view EXCEPT ![c] = {}]
    /\ dialog' = [dialog EXCEPT ![c] = NoDialog]
    /\ done' = [done EXCEPT ![c] = {}]
    /\ UNCHANGED <<next, req, up, file, stores, replies, restarts>> /\ UNCHANGED env /\ UNCHANGED mem /\ Quiet

Connect(c) ==
    /\ ~conn[c]
    /\ conn' = [conn EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<next, req, view, dialog, done, up, file, stores, replies, restarts>> /\ UNCHANGED env
    /\ UNCHANGED mem /\ Quiet

\* No request of r's account after r has been open, held, stored or expired: each asked since was refused (at its ask,
\* or cancelled). Then a late answer for r is still what the person meant its account's file to hold.
Latest(r) == \A q \in Ids : (q > r /\ req[q].acct = req[r].acct) => req[q].st \in {"unused", "refused"}

\* An answer the server takes: one for an open request, or for an expired one that is still the newest of its account.
Takes(m) == m.kind = "secret" /\ (req[m.id].st = "open" \/ (req[m.id].st = "expired" /\ Latest(m.id)))
Good(v) == v \notin BadValues

\* Value v is in request r's file, durably: stored; the asker hears "stored" unless answered already (it expired).
StoreValue(r, v) ==
    /\ req' = [req EXCEPT ![r].st = "stored"]
    /\ file' = [file EXCEPT ![req[r].acct] = v]
    /\ stores' = [stores EXCEPT ![r] = @ + 1]
    /\ replies' = IF Replied(r) THEN replies ELSE replies \cup {Reply(r, "stored", "none")}
    /\ transcript' = {[id |-> r, what |-> "stored", acct |-> req[r].acct]}
    /\ log' = {[id |-> r, event |-> "stored"]}

StoreOpen(m) == StoreValue(m.id, m.v) /\ UNCHANGED held

\* The write was not durable: the value is held in memory, its file as it was, or holding it already (the rename made,
\* the directory's sync failed). The asker hears nothing yet; its session and the log say it is held.
Hold(r, v) ==
    /\ req' = [req EXCEPT ![r].st = "holding"]
    /\ held' = [held EXCEPT ![r] = v]
    /\ file' \in {file, [file EXCEPT ![req[r].acct] = v]}
    /\ transcript' = {[id |-> r, what |-> "holding", acct |-> req[r].acct]}
    /\ log' = {[id |-> r, event |-> "holding"]}
    /\ UNCHANGED <<stores, replies>>

\* The first write of a value the server takes.
Write(r, v) == IF disk = "ok" THEN StoreValue(r, v) /\ UNCHANGED held ELSE Hold(r, v)

\* What the server does with an answer it takes: a value not of the file's shape is refused (malformed) and asked again;
\* for an expired request, whose asker was answered already, it is only logged. Any other value is written.
Accept(m) ==
    IF ~Good(m.v)
    THEN IF req[m.id].st = "open"
         THEN RefuseOpen(m.id, "malformed") /\ UNCHANGED <<file, stores, held>>
         ELSE /\ log' = {[id |-> m.id, event |-> "malformed"]} /\ transcript' = {}
              /\ UNCHANGED <<req, file, stores, replies, held>>
    ELSE Write(m.id, m.v)

DropLate(m) ==
    /\ log' = {[id |-> m.id, event |-> "late"]}
    /\ UNCHANGED <<req, file, stores, replies, held>> /\ transcript' = {}

\* The server applies an answer, against the request's state when it processes it. St(m) is what it does with an answer
\* it takes, Late(m) with one it does not. A value of the right shape that it takes is taken: it must never be dropped.
ReceiveWith(St(_), Late(_), m) ==
    /\ m \in up
    /\ up' = up \ {m}
    /\ taken' = IF Takes(m) /\ Good(m.v) THEN [taken EXCEPT ![m.id] = m.v] ELSE taken
    /\ IF Takes(m) THEN St(m)
       ELSE IF req[m.id].st = "open" /\ m.kind = "cancel"
            THEN RefuseOpen(m.id, "cancelled") /\ UNCHANGED <<file, stores, held>>
            ELSE Late(m)
    /\ UNCHANGED <<next, conn, view, dialog, done, restarts, lost>> /\ UNCHANGED env

Receive(m) == ReceiveWith(Accept, DropLate, m)

\* A held value is written again. Retry: the disk takes it, so it is stored, and the server's memory of it wiped.
\* RetryFails: it does not, and the value stays held (its file may hold it already, its durability unconfirmed).
Retry(r) ==
    /\ req[r].st = "holding" /\ disk = "ok"
    /\ StoreValue(r, held[r])
    /\ held' = [held EXCEPT ![r] = NoValue]
    /\ UNCHANGED <<next, conn, view, dialog, done, up, restarts, taken, lost>> /\ UNCHANGED env

RetryFails(r) ==
    /\ req[r].st = "holding" /\ disk = "failing"
    /\ file' \in {file, [file EXCEPT ![req[r].acct] = held[r]]}
    /\ log' = {[id |-> r, event |-> "holding"]} /\ transcript' = {}
    /\ UNCHANGED <<next, req, conn, view, dialog, done, up, stores, replies, restarts>> /\ UNCHANGED env
    /\ UNCHANGED mem

\* The disk starts failing (full, its directory made unwritable), and is fixed.
DiskFails ==
    /\ disk = "ok" /\ failures < MaxFailures
    /\ disk' = "failing" /\ failures' = failures + 1
    /\ UNCHANGED <<next, req, conn, view, dialog, done, up, file, stores, replies, restarts>> /\ UNCHANGED mem
    /\ Quiet

DiskHeals ==
    /\ disk = "failing" /\ disk' = "ok"
    /\ UNCHANGED <<next, req, conn, view, dialog, done, up, file, stores, replies, restarts, failures>>
    /\ UNCHANGED mem /\ Quiet

\* The request's deadline passes with no answer: its asker is refused (timeout), and it expires.
Timeout(r) ==
    /\ req[r].st = "open"
    /\ RefuseAs(r, "expired", "timeout")
    /\ UNCHANGED <<next, conn, view, dialog, done, up, file, stores, restarts>> /\ UNCHANGED env /\ UNCHANGED mem

\* The server ends (a crash or a stop) and starts again. Requests are not durable: each open one is dropped, unanswered,
\* its asker gone with the old server; each held value is lost with the server's memory (logged); an expired one is
\* forgotten, so a late answer for it is late. The id counter is durable. Windows keep their stale views until they
\* sync.
RestartWith(N(_)) ==
    /\ restarts < MaxRestarts
    /\ restarts' = restarts + 1
    /\ req' = [r \in Ids |-> CASE req[r].st \in {"open", "holding"} -> [req[r] EXCEPT !.st = "dropped"]
                               [] req[r].st = "expired" -> [req[r] EXCEPT !.st = "refused"]
                               [] OTHER -> req[r]]
    /\ next' = N(next)
    /\ held' = [r \in Ids |-> NoValue]
    /\ lost' = lost \cup HoldingIds
    /\ log' = {[id |-> r, event |-> "dropped"] : r \in OpenIds} \cup {[id |-> r, event |-> "lost"] : r \in HoldingIds}
    /\ UNCHANGED <<conn, view, dialog, done, up, file, stores, replies, taken>> /\ UNCHANGED env /\ transcript' = {}

Restart == RestartWith(LAMBDA n : n)

NextWith(A(_, _), S(_), R(_), T(_), Rs) ==
    \/ \E a \in Askers, acct \in Accounts : A(a, acct)
    \/ \E c \in Clients : S(c) \/ Submit(c) \/ Cancel(c) \/ Disconnect(c) \/ Connect(c)
    \/ \E c \in Clients, v \in Values : Type(c, v)
    \/ \E m \in up : R(m)
    \/ \E r \in Ids : T(r)
    \/ Rs

\* The held values written again, and the disk failing and being fixed.
RetryNext == \E r \in Ids : Retry(r) \/ RetryFails(r)
DiskNext == DiskFails \/ DiskHeals

Next == NextWith(Ask, Sync, Receive, Timeout, Restart) \/ RetryNext \/ DiskNext

\* Deadlines pass, windows receive the state, answers in flight arrive, a failing disk is fixed and a held value is
\* written again. Nothing makes a person answer.
Fairness ==
    /\ \A r \in Ids : WF_vars(Timeout(r))
    /\ \A c \in Clients : WF_vars(Sync(c))
    /\ WF_vars(\E m \in up : Receive(m))
    /\ WF_vars(DiskHeals)
    /\ \A r \in Ids : WF_vars(Retry(r))

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
    /\ disk \in {"ok", "failing"} /\ failures \in 0 .. MaxFailures
    /\ held \in [Ids -> Values \cup {NoValue}] /\ taken \in [Ids -> Values \cup {NoValue}] /\ lost \subseteq Ids

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
\* At most one request is open or held per account.
OneOpenPerAccount == \A q, r \in OpenIds \cup HoldingIds : req[q].acct = req[r].acct => q = r
\* Each asker is answered at most once per request: once its request ended stored, refused or expired, and not while it
\* is unused or open (a held one's asker is answered when it is stored, unless it expired first; a request dropped at a
\* restart has no asker left to answer).
AnsweredOnce == \A r \in Ids : Cardinality({x \in replies : x.id = r}) <= 1
AnsweredWhenEnded ==
    \A r \in Ids : /\ req[r].st \in {"stored", "refused", "expired"} => Replied(r)
                   /\ req[r].st \in {"unused", "open"} => ~Replied(r)

\* The one-chance rule (owner, 2026-11-14): a value of the right shape that the server received is never dropped while
\* the server runs. It is stored, or held for its next write, or it went with a restart (lost: the one exception).
ReceivedNeverDropped ==
    \A r \in Ids : taken[r] # NoValue =>
        \/ req[r].st = "stored" /\ stores[r] = 1
        \/ req[r].st = "holding" /\ held[r] = taken[r]
        \/ r \in lost
\* The asker hears "stored" only once the value is durable in its file.
StoredOnlyDurable == \A x \in replies : x.outcome = "stored" => stores[x.id] = 1
\* The server holds a value in memory only while its write is retried: once durable (or lost), the memory is wiped.
HeldOnlyWhileHolding == \A r \in Ids : held[r] # NoValue <=> req[r].st = "holding"
\* A value not of the file's shape never reaches an account's file.
NoMalformedStored == \A a \in Accounts : file[a] \notin BadValues

\* A closed request is never satisfied later, nor reopened, nor its id reused: once it has ended, it never changes.
ClosedStaysClosed ==
    [][\A r \in Ids : req[r].st \in {"stored", "refused", "dropped"} =>
                          req'[r] = req[r] /\ stores'[r] = stores[r]]_vars
\* A held value ends stored, or lost at a restart; an expired request is only stored, held, or forgotten at a restart.
HeldOnlyStored ==
    [][\A r \in Ids : /\ req[r].st = "holding" => req'[r].st \in {"holding", "stored", "dropped"}
                      /\ req[r].st = "expired" => req'[r].st \in {"expired", "holding", "stored", "refused"}]_vars

\* A request opens only while some connected window can take it; otherwise its asker is told "no-client" at once.
AskNeedsTaker == [][\A r \in Ids : req[r].st = "unused" /\ req'[r].st = "open" => Takers # {}]_vars

\* Every request ends: stored, refused, expired, held or dropped.
EveryRequestEnds == \A r \in Ids : req[r].st = "open" ~> req[r].st # "open"
\* Every held value is written in the end (stored), unless the server ends first.
HeldIsStored == \A r \in Ids : req[r].st = "holding" ~> req[r].st # "holding"
\* Every window's dialog for a request that has ended closes.
DialogsClose == \A c \in Clients, r \in Ids : (dialog[c].id = r /\ req[r].st # "open") ~> dialog[c].id # r
=============================================================================
