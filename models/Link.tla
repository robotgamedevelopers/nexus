------------------------------- MODULE Link -------------------------------
(***************************************************************************)
(* What nexus adds on top of mosh's State Synchronization Protocol, between *)
(* the windows and nexus-server (src/sync, src/link, src/server).           *)
(*                                                                         *)
(* Up: each window's actions are an append-only list, numbered from 1      *)
(* (src/sync/actions.h): a send, a queue or a stop, each naming a session;  *)
(* a create, which makes a session for an AI (copying another's         *)
(* settings, or the AI's defaults); and an ensure, which makes one only *)
(* if the AI has none. mosh carries the list as a state; what reaches  *)
(* the server is a range of it, first..last, that may repeat a range it    *)
(* had, come after a later one, or (if mosh ever did) start past what the  *)
(* server has: Deliver takes any range. The server's Applier applies each  *)
(* number once, in order, and refuses a range that would skip one.         *)
(*                                                                         *)
(* The server's sessions (src/sync/hub.h), each by its id: the AI it   *)
(* is with (an attribute only), the transcript, the queue, and the         *)
(* AI's entry still growing, the open entry, kept in memory only       *)
(* (Hub::setOpen). The server mints ids from 1, from a durable counter,    *)
(* never reusing one; a session is never removed. An action naming a       *)
(* session the server does not hold changes nothing (it is counted, and    *)
(* skipped). The server's own steps: Grow (the open entry is made, or      *)
(* grows), Close (it is closed: the closed entry goes into the transcript, *)
(* durable, and the open one goes), Take (with no entry open, the queue's  *)
(* oldest prompt is delivered: it leaves the queue and goes into the       *)
(* transcript as the person's entry), and Restart (the server ends and     *)
(* starts again from its store: the sessions, the id counter, the          *)
(* transcripts, the queues and the Appliers' counters are durable; an open *)
(* entry is lost, and the turn it belonged to is cut with a notice). A stop *)
(* changes no session: it reaches the session's responder (the driver      *)
(* supervisor) once, or is refused for a session the server does not hold. *)
(*                                                                         *)
(* Down: what the server sends is each session's AI, its transcript's  *)
(* tail, the newest W entries, then the open entry, which the tail does    *)
(* not count, and the whole queue (src/sync/sessions.h). Nothing names a   *)
(* tab. Every change makes a new state, kept in `snaps` in order. mosh is  *)
(* abstracted to what nexus relies on: a window's receiver eventually holds *)
(* the newest state; it may skip states between; it never goes back to an *)
(* older one. Receive(c, j) is the window receiving snaps[j], the same one  *)
(* again or a later one. Each window has one tab per AI; which session *)
(* a tab shows is the window's own business (shown): the AI's first    *)
(* session, the lowest id, fixed once the window has seen one. The window's *)
(* mirror (src/sync/mirror.h) adds to the tab's transcript the closed      *)
(* entries of that session it has not shown, with a notice "N earlier      *)
(* entries not shown" in place of entries that left the tail before it saw  *)
(* them; it holds the open entry, if any (live), apart from its            *)
(* transcript; and it makes the tab's queue the state's.                   *)
(*                                                                         *)
(* Not modelled: mosh itself (its sockets, crypto, fragments, timers,      *)
(* acknowledgements: mosh's own; after a restart a window's new session     *)
(* starts from the empty state, which the mirror sees as a later state);    *)
(* the hub's limits but the number of sessions (a refused action is a      *)
(* notice in the transcript, which the mirror shows like any entry); the   *)
(* tail's byte budget (the tail here is by count only); a session's        *)
(* harness, account, model and effort, and the actions that set them,      *)
(* which change no transcript or queue but for the notice of a refused one *)
(* (unit-tested; Supervisor.tla has what a change does to a turn); the     *)
(* turns themselves (Supervisor.tla); a session's activity, the state      *)
(* cursor's (Supervisor.tla ActivityOf; its carriage is unit-tested).      *)
(*                                                                         *)
(* Entries: a prompt is [k |-> "P", text |-> <<c, n>>], action n of window  *)
(* c (sent, or queued and delivered); its placeholder reply [k |-> "F",     *)
(* text |-> <<>>]; the AI's grown text, open [k |-> "O", text |->       *)
(* <<g, len>>] and closed [k |-> "G", text |-> <<g, len>>], the g-th the    *)
(* server made, of len pieces; the notice of a turn cut by a restart, which *)
(* lost open entry g, [k |-> "X", text |-> <<g>>]; the mirror's notice of m *)
(* entries not shown [k |-> "N", text |-> <<m>>]. A queued prompt is        *)
(* <<c, n>>.                                                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC

CONSTANTS
    Clients,      \* the windows, 1..N
    AIs,      \* the AIs: each window has a tab for each
    Start,        \* the AIs of the sessions the server holds at the start, ids 1..Len(Start) (an older store's)
    MaxSessions,  \* the most sessions the server holds (the hub's limit): ids run 1..MaxSessions
    MaxActions,   \* how many actions each window makes, at most
    W,            \* the tail: how many of a transcript's newest entries a state carries
    MaxGrows,     \* how many times the server grows an open entry, in all
    MaxRestarts,  \* how many times the server restarts
    Kinds         \* the kinds of action the windows make: of "Send", "Queue", "Stop", "Create" and "Ensure"

ASSUME /\ W >= 1
       /\ Kinds \subseteq {"Send", "Queue", "Stop", "Create", "Ensure"}
       /\ Len(Start) <= MaxSessions
       /\ \A i \in 1 .. Len(Start) : Start[i] \in AIs

Ids == 1 .. MaxSessions

VARIABLES
    acts,       \* acts[c]: window c's list, each [kind, sid, ai]; action n is acts[c][n]
    applied,    \* applied[c]: the server's Applier for window c: the last number applied
    done,       \* done[c]: every number the server applied for window c, in the order applied (a ghost)
    ses,        \* ses[i]: session i as the server holds it (durable): NoSes, or [ai |-> f]
    nextSid,    \* the next id the server mints (durable)
    minted,     \* every session minted, in order: [id |-> i, a |-> <<c, n>> the action, or <<0, 0>> at the start]
    skipped,    \* the actions applied that changed nothing, each <<c, n>>: a ghost
    tr,         \* tr[i]: the server's transcript of session i (durable)
    q,          \* q[i]: the server's queue of session i (durable)
    op,         \* op[i]: the server's open entry of session i, None or [g, len] (memory only)
    stops,      \* stops[i]: every stop applied naming session i: [a |-> <<c, n>>, ok |-> it reached the responder]
    made,       \* how many open entries the server has made, and grown, and restarted: [g, grows, restarts]
    snaps,      \* the server's states, oldest first; snaps[1] is the one it starts with
    last,       \* last[c]: the index in snaps of the state window c's receiver holds
    shown,      \* shown[c][f]: the session window c's tab of AI f shows, 0 for none yet (the window's own)
    view,       \* view[c][f]: the transcript in window c's tab of AI f
    vq,         \* vq[c][f]: its queue
    seen,       \* seen[c][f]: its mirror: [next |-> the transcript index it shows through]
    live        \* live[c][f]: the open entry its mirror holds, or None

vars == <<acts, applied, done, ses, nextSid, minted, skipped, tr, q, op, stops, made, snaps, last, shown, view, vq,
          seen, live>>
server == <<ses, nextSid, minted, skipped, tr, q, op, stops, made, snaps>>
window == <<last, shown, view, vq, seen, live>>

None == [k |-> "none", text |-> <<>>]
NoSes == [ai |-> ""]
Max(a, b) == IF a > b THEN a ELSE b
Prompt(c, n) == [k |-> "P", text |-> <<c, n>>]
Reply == [k |-> "F", text |-> <<>>]
Notice(m) == [k |-> "N", text |-> <<m>>]
Open(o) == [k |-> "O", text |-> <<o.g, o.len>>]
Grown(o) == [k |-> "G", text |-> <<o.g, o.len>>]
Cut(o) == [k |-> "X", text |-> <<o.g>>]
NoOpen == [g |-> 0, len |-> 0]
IsPrefix(a, b) == Len(a) <= Len(b) /\ SubSeq(b, 1, Len(a)) = a

\* A session the server holds.
Held(s, i) == i \in Ids /\ s[i] # NoSes

\* What the server sends of its sessions: for each it holds, its AI, its transcript by its newest W entries from
\* index `first` (from 0), then the open entry, if any, which the tail does not count; and the whole queue. A session
\* it does not hold is NoSnap.
NoSnap == [ai |-> "", first |-> 0, entries |-> <<>>, queue |-> <<>>]
OpenOf(ops, i) == IF ops[i] = NoOpen THEN <<>> ELSE << Open(ops[i]) >>
TailFirst(s) == Max(0, Len(s) - W)
Snap(sess, trs, qs, ops) ==
    [i \in Ids |-> IF sess[i] = NoSes THEN NoSnap
                   ELSE LET s == trs[i] IN
                        [ai  |-> sess[i].ai,
                         first   |-> TailFirst(s),
                         entries |-> SubSeq(s, TailFirst(s) + 1, Len(s)) \o OpenOf(ops, i),
                         queue   |-> qs[i]]]

\* A new state when the sessions changed: the code compares the state it makes with the last.
Push(sess, trs, qs, ops) ==
    LET s == Snap(sess, trs, qs, ops) IN IF s = snaps[Len(snaps)] THEN snaps ELSE Append(snaps, s)

Empty == [i \in Ids |-> <<>>]
StartSes == [i \in Ids |-> IF i <= Len(Start) THEN [ai |-> Start[i]] ELSE NoSes]

Init ==
    /\ acts = [c \in Clients |-> <<>>]
    /\ applied = [c \in Clients |-> 0]
    /\ done = [c \in Clients |-> <<>>]
    /\ ses = StartSes
    /\ nextSid = Len(Start) + 1
    /\ minted = [k \in 1 .. Len(Start) |-> [id |-> k, a |-> <<0, 0>>]]
    /\ skipped = {}
    /\ tr = Empty
    /\ q = Empty
    /\ op = [i \in Ids |-> NoOpen]
    /\ stops = Empty
    /\ made = [g |-> 0, grows |-> 0, restarts |-> 0]
    /\ snaps = << Snap(StartSes, Empty, Empty, [i \in Ids |-> NoOpen]) >>
    /\ last = [c \in Clients |-> 1]
    /\ shown = [c \in Clients |-> [f \in AIs |-> 0]]
    /\ view = [c \in Clients |-> [f \in AIs |-> <<>>]]
    /\ vq = [c \in Clients |-> [f \in AIs |-> <<>>]]
    /\ seen = [c \in Clients |-> [f \in AIs |-> [next |-> 0]]]
    /\ live = [c \in Clients |-> [f \in AIs |-> None]]

\* The actions a window may make: a send, a queue or a stop naming any id (held or not); a create for an AI, from
\* no session (0: the AI's defaults) or any id; an ensure for an AI.
Targets ==
    {[kind |-> k, sid |-> i, ai |-> ""] : k \in Kinds \cap {"Send", "Queue", "Stop"}, i \in Ids} \cup
    {[kind |-> "Create", sid |-> i, ai |-> f] : i \in (IF "Create" \in Kinds THEN 0 .. MaxSessions ELSE {}),
                                                    f \in AIs} \cup
    {[kind |-> "Ensure", sid |-> 0, ai |-> f] : f \in (IF "Ensure" \in Kinds THEN AIs ELSE {})}

\* Window c makes an action: the next number in its list.
Act(c, a) ==
    /\ Len(acts[c]) < MaxActions
    /\ a \in Targets
    /\ acts' = [acts EXCEPT ![c] = Append(@, a)]
    /\ UNCHANGED <<applied, done, server, window>>

\* The hub applies action n of window c (src/sync/hub.cpp) to the server's state r = [trs, qs, sts, ses, nxt, mint,
\* skip]: a send puts the prompt and its placeholder reply in the session's transcript, a queue puts the prompt in its
\* queue, a stop reaches its responder; each only for a session the server holds. A create mints the next id for its
\* AI, unless the server holds its limit or the session it copies from does not exist; an ensure does the same
\* only when the AI has no session. Whatever changes nothing is skipped.
Mint(r, f, c, n) ==
    [r EXCEPT !.ses[r.nxt] = [ai |-> f], !.nxt = @ + 1, !.mint = Append(@, [id |-> r.nxt, a |-> <<c, n>>])]
Skip(r, c, n) == [r EXCEPT !.skip = @ \cup {<<c, n>>}]
HasSession(s, f) == \E i \in Ids : s[i] # NoSes /\ s[i].ai = f
ApplyOneWith(Mt(_, _, _, _), c, n, r) ==
    LET a == acts[c][n]
        held == Held(r.ses, a.sid)
        full == r.nxt > MaxSessions
    IN
    CASE a.kind = "Send"   -> IF held THEN [r EXCEPT !.trs[a.sid] = @ \o << Prompt(c, n), Reply >>] ELSE Skip(r, c, n)
      [] a.kind = "Queue"  -> IF held THEN [r EXCEPT !.qs[a.sid] = Append(@, <<c, n>>)] ELSE Skip(r, c, n)
      [] a.kind = "Stop"   -> [r EXCEPT !.sts[a.sid] = Append(@, [a |-> <<c, n>>, ok |-> held])]
      [] a.kind = "Create" -> IF full \/ (a.sid # 0 /\ ~held) THEN Skip(r, c, n) ELSE Mt(r, a.ai, c, n)
      [] OTHER             -> IF full \/ HasSession(r.ses, a.ai) THEN Skip(r, c, n) ELSE Mt(r, a.ai, c, n)
ApplyOne(c, n, r) == ApplyOneWith(Mint, c, n, r)

RECURSIVE ApplyAll(_, _, _, _)
\* Actions from..to of window c, in order.
ApplyAll(c, from, to, r) ==
    IF from > to THEN r ELSE ApplyAll(c, from + 1, to, ApplyOne(c, from, r))

Numbers(from, to) == [i \in 1 .. (to - from + 1) |-> from + i - 1]

Server == [trs |-> tr, qs |-> q, sts |-> stops, ses |-> ses, nxt |-> nextSid, mint |-> minted, skip |-> skipped]

\* The server's state after r: everything the actions change, and a new state if the sessions changed.
Took(r) ==
    /\ tr' = r.trs
    /\ q' = r.qs
    /\ stops' = r.sts
    /\ ses' = r.ses
    /\ nextSid' = r.nxt
    /\ minted' = r.mint
    /\ skipped' = r.skip
    /\ snaps' = Push(r.ses, r.trs, r.qs, op)
    /\ UNCHANGED <<op, made>>

\* The server takes a range first..lst of window c's list (src/sync/actions.cpp, Applier::apply): one that starts past
\* applied + 1 is refused; otherwise the numbers past applied are applied, in order, and a new state is made if the
\* sessions changed. A range wholly applied before changes nothing. (A witness replaces Applier through DeliverWith.)
ApplyRange(c, from, to) ==
    /\ applied' = [applied EXCEPT ![c] = to]
    /\ done' = [done EXCEPT ![c] = @ \o Numbers(from, to)]
    /\ Took(ApplyAll(c, from, to, Server))

Applier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, server>>
    ELSE ApplyRange(c, applied[c] + 1, lst)

DeliverWith(Apply(_, _, _), c, first, lst) ==
    /\ 1 <= first /\ first <= lst /\ lst <= Len(acts[c])
    /\ Apply(c, first, lst)
    /\ UNCHANGED <<acts, window>>

Deliver(c, first, lst) == DeliverWith(Applier, c, first, lst)

\* The server's own steps: the AI's text grows (an open entry is made, or grows by a piece) in a session it holds
\* that has had something sent or queued: an AI replies only to a prompt.
Grow(i) ==
    /\ made.grows < MaxGrows
    /\ Held(ses, i) /\ (tr[i] # <<>> \/ q[i] # <<>>)
    /\ LET o == IF op[i] = NoOpen THEN [g |-> made.g + 1, len |-> 1] ELSE [op[i] EXCEPT !.len = @ + 1] IN
       /\ op' = [op EXCEPT ![i] = o]
       /\ made' = [made EXCEPT !.g = IF op[i] = NoOpen THEN @ + 1 ELSE @, !.grows = @ + 1]
       /\ snaps' = Push(ses, tr, q, op')
    /\ UNCHANGED <<acts, applied, done, ses, nextSid, minted, skipped, tr, q, stops, window>>

\* The open entry's block ends: the closed entry goes into the transcript (durable), with all its text.
CloseWith(C(_), i) ==
    /\ op[i] # NoOpen
    /\ tr' = [tr EXCEPT ![i] = Append(@, C(op[i]))]
    /\ op' = [op EXCEPT ![i] = NoOpen]
    /\ snaps' = Push(ses, tr', q, op')
    /\ UNCHANGED <<acts, applied, done, ses, nextSid, minted, skipped, q, stops, made, window>>
Close(i) == CloseWith(Grown, i)

\* A turn's end with no entry open: the queue's oldest prompt is delivered, as the person's entry.
TakeWith(Tk(_, _), i) ==
    /\ op[i] = NoOpen
    /\ q[i] # <<>>
    /\ LET r == Tk(tr[i], q[i]) IN
       /\ tr' = [tr EXCEPT ![i] = r.tr]
       /\ q' = [q EXCEPT ![i] = r.q]
    /\ snaps' = Push(ses, tr', q', op)
    /\ UNCHANGED <<acts, applied, done, ses, nextSid, minted, skipped, op, stops, made, window>>
Taken(trt, qt) == [tr |-> Append(trt, Prompt(Head(qt)[1], Head(qt)[2])), q |-> Tail(qt)]
Take(i) == TakeWith(Taken, i)

\* The server restarts from its store: the sessions and the id counter are kept; an open entry is lost, and its turn
\* is cut with a notice in the transcript.
RestartAll(Lost(_), Nx) ==
    /\ made.restarts < MaxRestarts
    /\ tr' = [i \in Ids |-> IF op[i] = NoOpen THEN tr[i] ELSE Lost(i)]
    /\ op' = [i \in Ids |-> NoOpen]
    /\ made' = [made EXCEPT !.restarts = @ + 1]
    /\ nextSid' = Nx
    /\ snaps' = Push(ses, tr', q, op')
    /\ UNCHANGED <<acts, applied, done, ses, minted, skipped, q, stops, window>>
RestartWith(Lost(_)) == RestartAll(Lost, nextSid)
CutLost(i) == Append(tr[i], Cut(op[i]))
Restart == RestartWith(CutLost)

\* The window's mirror, given the state s of the session a tab shows: a notice for the entries that left the tail
\* before it saw them, then the closed entries it has not shown (an open entry is always the newest, so they stop at
\* it), the open entry held apart, and the queue made the state's.
MirrorTab(v, qv, se, s) ==
    LET gap == s.first - se.next
        v1 == IF gap > 0 THEN Append(v, Notice(gap)) ELSE v
        from == Max(se.next, s.first)
        total == s.first + Len(s.entries)
        rest == SubSeq(s.entries, from - s.first + 1, Len(s.entries))
        closed == SelectSeq(rest, LAMBDA e : e.k # "O")
        next == Max(from + Len(closed), se.next)
        lv == IF next < total THEN s.entries[next - s.first + 1] ELSE None
    IN [view |-> v1 \o closed, vq |-> s.queue, seen |-> [next |-> next], live |-> lv]

\* The AI's first session in state s, the lowest id: 0 when it has none.
First(s, f) ==
    IF \E i \in Ids : s[i].ai = f
    THEN CHOOSE i \in Ids : s[i].ai = f /\ \A k \in Ids : s[k].ai = f => i <= k
    ELSE 0

\* Which session a tab shows: the one it showed, else the AI's first in the state received.
KeepFirst(cur, s, f) == IF cur # 0 THEN cur ELSE First(s, f)

ReceiveAll(Mirror(_, _, _, _), Show(_, _, _), c, j) ==
    /\ last[c] <= j /\ j <= Len(snaps)
    /\ LET sh == [f \in AIs |-> Show(shown[c][f], snaps[j], f)]
           m == [f \in AIs |->
                    IF sh[f] = 0
                    THEN [view |-> view[c][f], vq |-> vq[c][f], seen |-> seen[c][f], live |-> live[c][f]]
                    ELSE Mirror(view[c][f], vq[c][f], seen[c][f], snaps[j][sh[f]])] IN
       /\ shown' = [shown EXCEPT ![c] = sh]
       /\ view' = [view EXCEPT ![c] = [f \in AIs |-> m[f].view]]
       /\ vq' = [vq EXCEPT ![c] = [f \in AIs |-> m[f].vq]]
       /\ seen' = [seen EXCEPT ![c] = [f \in AIs |-> m[f].seen]]
       /\ live' = [live EXCEPT ![c] = [f \in AIs |-> m[f].live]]
    /\ last' = [last EXCEPT ![c] = j]
    /\ UNCHANGED <<acts, applied, done, server>>

ReceiveWith(Mirror(_, _, _, _), c, j) == ReceiveAll(Mirror, KeepFirst, c, j)

Receive(c, j) == ReceiveWith(MirrorTab, c, j)

NextWithAll(D(_, _, _), R(_, _), Cl(_), Tk(_), Rs) ==
    \/ \E c \in Clients, a \in Targets : Act(c, a)
    \/ \E c \in Clients : \E f, l \in 1 .. Len(acts[c]) : D(c, f, l)
    \/ \E c \in Clients : \E j \in 1 .. Len(snaps) : R(c, j)
    \/ \E i \in Ids : Grow(i) \/ Cl(i) \/ Tk(i)
    \/ Rs

NextWith(D(_, _, _), R(_, _)) == NextWithAll(D, R, Close, Take, Restart)

Next == NextWith(Deliver, Receive)

\* mosh's promise, the fairness the liveness below needs: what a window has sent reaches the server whole, and the
\* newest state reaches every window. The server's own steps need none: Settled holds whatever they left.
DeliverRest(c) == Deliver(c, 1, Len(acts[c]))
ReceiveLatest(c) == Receive(c, Len(snaps))
Fairness == \A c \in Clients : WF_vars(DeliverRest(c)) /\ WF_vars(ReceiveLatest(c))

Spec == Init /\ [][Next]_vars /\ Fairness
\* The spec without the fairness, which a finite trace of the code (LinkTrace.tla) is checked against.
SafetySpec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* Properties

TypeOK ==
    /\ nextSid \in 1 .. (MaxSessions + 1)
    /\ \A c \in Clients :
        /\ applied[c] \in 0 .. Len(acts[c])
        /\ last[c] \in 1 .. Len(snaps)
        /\ \A f \in AIs : /\ shown[c][f] \in 0 .. MaxSessions
                              /\ shown[c][f] # 0 => seen[c][f].next \in 0 .. Len(tr[shown[c][f]])
    /\ \A i \in Ids : op[i] = NoOpen \/ (op[i].g \in 1 .. made.g /\ op[i].len >= 1)

\* Ids are minted from 1 in order and never reused: the server holds exactly the ids below the counter, each minted
\* once. The counter never goes down, across restarts, and a session, once made, is never removed or changed.
IdsNeverReused ==
    /\ Len(minted) = nextSid - 1
    /\ \A k \in 1 .. Len(minted) : minted[k].id = k
    /\ \A i \in Ids : (ses[i] # NoSes) = (i < nextSid)
SessionsKept == [][nextSid' >= nextSid /\ \A i \in Ids : ses[i] # NoSes => ses'[i] = ses[i]]_vars

\* Up: the server applied window c's actions 1, 2, ..., applied[c], each once, in order, and none it was not given.
ExactlyOnce ==
    \A c \in Clients :
        /\ done[c] = Numbers(1, applied[c])
        /\ applied[c] <= Len(acts[c])

\* Every action applied is on the server exactly once, where its kind puts it, unless it was skipped: a send in its
\* session's transcript; a queue in its queue or, delivered, in its transcript, never both; a stop among its session's
\* stops, reaching the responder exactly when the server held the session; a create or an ensure as one session
\* minted, for its AI. None not applied is.
RECURSIVE Count(_, _)
Count(x, s) == IF s = <<>> THEN 0 ELSE (IF Head(s) = x THEN 1 ELSE 0) + Count(x, Tail(s))
StopsOf(i) == [k \in 1 .. Len(stops[i]) |-> stops[i][k].a]
MintedBy == [k \in 1 .. Len(minted) |-> minted[k].a]
ServerHoldsEach ==
    /\ \A c \in Clients : \A n \in 1 .. Len(acts[c]) :
        LET a == acts[c][n]
            want == IF n <= applied[c] /\ <<c, n>> \notin skipped THEN 1 ELSE 0
        IN /\ a.kind \in {"Create", "Ensure"} => Count(<<c, n>>, MintedBy) = want
           /\ a.kind \notin {"Create", "Ensure"} => Count(<<c, n>>, MintedBy) = 0
           /\ \A i \in Ids :
              LET here == IF i = a.sid THEN want ELSE 0 IN
              CASE a.kind = "Send"  -> Count(Prompt(c, n), tr[i]) = here
                [] a.kind = "Queue" -> Count(Prompt(c, n), tr[i]) + Count(<<c, n>>, q[i]) = here
                [] a.kind = "Stop"  -> Count(<<c, n>>, StopsOf(i)) = IF i = a.sid /\ n <= applied[c] THEN 1 ELSE 0
                [] OTHER            -> TRUE
    /\ \A k \in 1 .. Len(minted) :
        minted[k].a # <<0, 0>> => ses[minted[k].id].ai = acts[minted[k].a[1]][minted[k].a[2]].ai

\* An ensure makes a session only for an AI that has none: no session it minted shares its AI with one made
\* before it.
EnsureMakesOne ==
    \A k \in 1 .. Len(minted) :
        LET a == minted[k].a IN
        a # <<0, 0>> /\ acts[a[1]][a[2]].kind = "Ensure" =>
            \A k2 \in 1 .. (k - 1) : ses[minted[k2].id].ai # ses[minted[k].id].ai

\* Each open entry the server makes ends in the transcript at most once: closed, or cut by a restart, never both.
EachClosedOnce ==
    \A i \in Ids : \A g \in 1 .. made.g :
        LET closed == {k \in 1 .. Len(tr[i]) : tr[i][k].k \in {"G", "X"} /\ tr[i][k].text[1] = g} IN
        /\ \A k, k2 \in closed : k = k2
        /\ op[i] # NoOpen /\ op[i].g = g => closed = {}

\* A window's transcript with each notice of m entries replaced by m holes: what it shows, by transcript index.
Hole == [k |-> "hole", text |-> <<>>]
RECURSIVE Expand(_)
Expand(v) ==
    IF v = <<>> THEN <<>>
    ELSE LET e == Head(v) IN
         (IF e.k = "N" THEN [i \in 1 .. e.text[1] |-> Hole] ELSE << e >>) \o Expand(Tail(v))

\* Down: each tab shows the transcript of the one session it shows, through what it has seen, each entry at its index,
\* or a hole the notice before it accounts for; never an entry twice, never one out of place, never one still open,
\* never one of another session. A tab that shows no session yet shows nothing. Its queue is that session's queue in
\* the state the window holds.
ViewMatches ==
    \A c \in Clients : \A f \in AIs :
        LET x == Expand(view[c][f])
            i == shown[c][f]
        IN IF i = 0
           THEN view[c][f] = <<>> /\ seen[c][f].next = 0
           ELSE /\ Len(x) = seen[c][f].next
                /\ Len(x) <= Len(tr[i])
                /\ \A k \in 1 .. Len(x) : x[k] = Hole \/ x[k] = tr[i][k]
QueueMatches ==
    \A c \in Clients : \A f \in AIs :
        vq[c][f] = IF shown[c][f] = 0 THEN <<>> ELSE snaps[last[c]][shown[c][f]].queue
\* A tab shows its AI's first session, and only that one, once it has seen it: the window's choice never moves.
ShowsFirst ==
    \A c \in Clients : \A f \in AIs :
        shown[c][f] # 0 => /\ ses[shown[c][f]].ai = f
                           /\ \A i \in Ids : ses[i].ai = f => shown[c][f] <= i
\* No two notices in a row, and none empty: a notice stands for a run of entries the window did not get.
NoticesSound ==
    \A c \in Clients : \A f \in AIs : \A k \in 1 .. Len(view[c][f]) :
        view[c][f][k].k = "N" => /\ view[c][f][k].text[1] > 0
                                 /\ k > 1 => view[c][f][k - 1].k # "N"

\* The open entry a window holds is text the server had: the open entry still growing, at least as long, or the
\* entry it closed into, or the notice of the restart that lost it. A window never shows text the server never had.
LiveSound ==
    \A c \in Clients : \A f \in AIs :
        LET e == live[c][f]
            i == shown[c][f]
        IN e # None =>
            /\ e.k = "O"
            /\ i # 0
            /\ \/ op[i] # NoOpen /\ op[i].g = e.text[1] /\ op[i].len >= e.text[2]
               \/ \E k \in 1 .. Len(tr[i]) : \/ tr[i][k].k = "G" /\ tr[i][k].text[1] = e.text[1]
                                                               /\ tr[i][k].text[2] >= e.text[2]
                                            \/ tr[i][k] = [k |-> "X", text |-> <<e.text[1]>>]

\* The server's open entry only grows, and goes only into the transcript: closed with all its text, or cut.
OpenGrows ==
    [][\A i \in Ids :
        op[i] # NoOpen =>
            \/ op'[i] # NoOpen /\ op'[i].g = op[i].g /\ op'[i].len >= op[i].len
            \/ op'[i] = NoOpen /\ (tr'[i] = Append(tr[i], Grown(op[i])) \/ tr'[i] = Append(tr[i], Cut(op[i])))]_vars

\* The server's transcripts only grow; its queues only grow at their end, or lose their oldest prompt, one at a time,
\* to the transcript, as the person's entry; every window's transcript only grows, and a window's receiver never goes
\* back.
QueueStep(i) ==
    \/ IsPrefix(q[i], q'[i])
    \/ /\ q[i] # <<>>
       /\ q'[i] = Tail(q[i])
       /\ tr'[i] = Append(tr[i], Prompt(Head(q[i])[1], Head(q[i])[2]))
OnlyGrow ==
    [][/\ \A i \in Ids : IsPrefix(tr[i], tr'[i]) /\ QueueStep(i) /\ IsPrefix(stops[i], stops'[i])
       /\ \A c \in Clients : /\ \A f \in AIs : IsPrefix(view[c][f], view'[c][f])
                             /\ last[c] <= last'[c]
                             /\ IsPrefix(done[c], done'[c])]_vars

\* Liveness: once the windows stop acting, the server applies every action and every window shows all of it: each tab
\* the AI's first session, every closed entry, the open entry, and the queue.
Settled ==
    \A c \in Clients :
        /\ applied[c] = Len(acts[c])
        /\ \A f \in AIs :
            LET i == shown[c][f] IN
            /\ i = First(ses, f)
            /\ i # 0 => /\ seen[c][f].next = Len(tr[i])
                        /\ vq[c][f] = q[i]
                        /\ live[c][f] = IF op[i] = NoOpen THEN None ELSE Open(op[i])
Converges == <>[]Settled

=============================================================================
