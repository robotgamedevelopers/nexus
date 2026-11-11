------------------------------- MODULE Link -------------------------------
(***************************************************************************)
(* What nexus adds on top of mosh's State Synchronization Protocol, between *)
(* the windows and nexus-server (src/sync, src/link, src/server).           *)
(*                                                                         *)
(* Up: each window's actions are an append-only list, numbered from 1      *)
(* (src/sync/actions.h): a send, a queue, or a stop. mosh carries the list *)
(* as a state; what reaches the server is a range of it, first..last, that  *)
(* may repeat a range it had, come after a later one, or (if mosh ever     *)
(* did) start past what the server has: Deliver takes any range. The       *)
(* server's Applier applies each number once, in order, and refuses a      *)
(* range that would skip one.                                              *)
(*                                                                         *)
(* The server's tabs (src/sync/hub.h): per tab, the transcript, the queue,  *)
(* and the friend's entry still growing, the open entry, kept in memory     *)
(* only (Hub::setOpen). The server's own steps: Grow (the open entry is     *)
(* made, or grows), Close (it is closed: the closed entry goes into the     *)
(* transcript, durable, and the open one goes), Take (with no entry open,   *)
(* the queue's oldest prompt is delivered: it leaves the queue and goes     *)
(* into the transcript as the person's entry), and Restart (the server      *)
(* ends and starts again from its store: the transcript, the queue and the  *)
(* Appliers' counters are durable; an open entry is lost, and the turn it   *)
(* belonged to is cut with a notice). A stop changes no tab: it reaches the *)
(* tab's responder (the driver supervisor) once, or is refused for a tab    *)
(* the server does not hold.                                               *)
(*                                                                         *)
(* Down: what the server sends is each transcript's tail, the newest W      *)
(* entries, then the open entry, which the tail does not count, and the    *)
(* whole queue (src/sync/tabs.h). Every change makes a new state, kept in  *)
(* `snaps` in order. mosh is abstracted to what nexus relies on: a         *)
(* window's receiver eventually holds the newest state; it may skip states *)
(* between; it never goes back to an older one. Receive(c, j) is the       *)
(* window receiving snaps[j], the same one again or a later one, and the   *)
(* window's mirror                                                         *)
(* (src/sync/mirror.h): it adds to its own transcript the closed entries it *)
(* has not shown, with a notice "N earlier entries not shown" in place of   *)
(* entries that left the tail before it saw them; it holds the open entry,  *)
(* if any (live), apart from its transcript; and it makes its queue the     *)
(* state's.                                                                *)
(*                                                                         *)
(* Not modelled: mosh itself (its sockets, crypto, fragments, timers,      *)
(* acknowledgements: mosh's own; after a restart a window's new session     *)
(* starts from the empty state, which the mirror sees as a later state);    *)
(* the hub's limits (a refused action is a notice in the transcript, which  *)
(* the mirror shows like any entry); the tail's byte budget (the tail here  *)
(* is by count only); the turns themselves (Supervisor.tla).               *)
(*                                                                         *)
(* Entries: a prompt is [k |-> "P", text |-> <<c, n>>], action n of window  *)
(* c (sent, or queued and delivered); its placeholder reply [k |-> "F",     *)
(* text |-> <<>>]; the friend's grown text, open [k |-> "O", text |->       *)
(* <<g, len>>] and closed [k |-> "G", text |-> <<g, len>>], the g-th the    *)
(* server made, of len pieces; the notice of a turn cut by a restart, which *)
(* lost open entry g, [k |-> "X", text |-> <<g>>]; the mirror's notice of m *)
(* entries not shown [k |-> "N", text |-> <<m>>]. A queued prompt is        *)
(* <<c, n>>.                                                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC

CONSTANTS
    Clients,      \* the windows, 1..N
    TabNames,     \* the friends' tabs
    MaxActions,   \* how many actions each window makes, at most
    W,            \* the tail: how many of a transcript's newest entries a state carries
    MaxGrows,     \* how many times the server grows an open entry, in all
    MaxRestarts,  \* how many times the server restarts
    Kinds         \* the kinds of action the windows make: of "Send", "Queue" and "Stop"

ASSUME W >= 1 /\ Kinds \subseteq {"Send", "Queue", "Stop"}

VARIABLES
    acts,       \* acts[c]: window c's list, each [kind, tab]; action n is acts[c][n]
    applied,    \* applied[c]: the server's Applier for window c: the last number applied
    done,       \* done[c]: every number the server applied for window c, in the order applied (a ghost)
    tr,         \* tr[t]: the server's transcript of tab t (durable)
    q,          \* q[t]: the server's queue of tab t (durable)
    op,         \* op[t]: the server's open entry of tab t, None or [g, len] (memory only)
    stops,      \* stops[t]: every stop applied to tab t, in order: [a |-> <<c, n>>, ok |-> it reached the responder]
    made,       \* how many open entries the server has made, and grown, and restarted: [g, grows, restarts]
    snaps,      \* the server's states, oldest first; snaps[1] is the empty one it starts with
    last,       \* last[c]: the index in snaps of the state window c's receiver holds
    view,       \* view[c][t]: window c's transcript of tab t
    vq,         \* vq[c][t]: window c's queue of tab t
    seen,       \* seen[c][t]: window c's mirror: [next |-> the transcript index it shows through]
    live        \* live[c][t]: the open entry window c's mirror holds for tab t, or None

vars == <<acts, applied, done, tr, q, op, stops, made, snaps, last, view, vq, seen, live>>
server == <<tr, q, op, stops, made, snaps>>

None == [k |-> "none", text |-> <<>>]
Max(a, b) == IF a > b THEN a ELSE b
Prompt(c, n) == [k |-> "P", text |-> <<c, n>>]
Reply == [k |-> "F", text |-> <<>>]
Notice(m) == [k |-> "N", text |-> <<m>>]
Open(o) == [k |-> "O", text |-> <<o.g, o.len>>]
Grown(o) == [k |-> "G", text |-> <<o.g, o.len>>]
Cut(o) == [k |-> "X", text |-> <<o.g>>]
NoOpen == [g |-> 0, len |-> 0]
IsPrefix(a, b) == Len(a) <= Len(b) /\ SubSeq(b, 1, Len(a)) = a

\* A tab the server holds: something was sent or queued to it.
Holds(trs, qs, t) == trs[t] # <<>> \/ qs[t] # <<>>

\* What the server sends of its tabs: each transcript by its newest W entries, from index `first` (from 0), then the
\* open entry, if any, which the tail does not count; and the whole queue.
OpenOf(ops, t) == IF ops[t] = NoOpen THEN <<>> ELSE << Open(ops[t]) >>
TailFirst(s) == Max(0, Len(s) - W)
Snap(trs, qs, ops) == [t \in TabNames |-> LET s == trs[t] IN
                                          [first   |-> TailFirst(s),
                                           entries |-> SubSeq(s, TailFirst(s) + 1, Len(s)) \o OpenOf(ops, t),
                                           queue   |-> qs[t]]]

\* A new state when the tabs changed: the code compares the state it makes with the last.
Push(trs, qs, ops) ==
    LET s == Snap(trs, qs, ops) IN IF s = snaps[Len(snaps)] THEN snaps ELSE Append(snaps, s)

Empty == [t \in TabNames |-> <<>>]

Init ==
    /\ acts = [c \in Clients |-> <<>>]
    /\ applied = [c \in Clients |-> 0]
    /\ done = [c \in Clients |-> <<>>]
    /\ tr = Empty
    /\ q = Empty
    /\ op = [t \in TabNames |-> NoOpen]
    /\ stops = Empty
    /\ made = [g |-> 0, grows |-> 0, restarts |-> 0]
    /\ snaps = << Snap(Empty, Empty, [t \in TabNames |-> NoOpen]) >>
    /\ last = [c \in Clients |-> 1]
    /\ view = [c \in Clients |-> Empty]
    /\ vq = [c \in Clients |-> Empty]
    /\ seen = [c \in Clients |-> [t \in TabNames |-> [next |-> 0]]]
    /\ live = [c \in Clients |-> [t \in TabNames |-> None]]

\* Window c makes an action: the next number in its list.
Act(c, kind, t) ==
    /\ Len(acts[c]) < MaxActions
    /\ acts' = [acts EXCEPT ![c] = Append(@, [kind |-> kind, tab |-> t])]
    /\ UNCHANGED <<applied, done, server, last, view, vq, seen, live>>

\* The hub applies action n of window c (src/sync/hub.cpp) to the server's tabs r = [trs, qs, sts]: a send puts the
\* prompt and its placeholder reply in the transcript, a queue puts the prompt in the queue, a stop reaches the tab's
\* responder if the server holds the tab and is refused otherwise.
ApplyOne(c, n, r) ==
    LET a == acts[c][n] IN
    CASE a.kind = "Send"  -> [r EXCEPT !.trs[a.tab] = @ \o << Prompt(c, n), Reply >>]
      [] a.kind = "Queue" -> [r EXCEPT !.qs[a.tab] = Append(@, <<c, n>>)]
      [] OTHER            -> [r EXCEPT !.sts[a.tab] = Append(@, [a |-> <<c, n>>, ok |-> Holds(r.trs, r.qs, a.tab)])]

RECURSIVE ApplyAll(_, _, _, _)
\* Actions from..to of window c, in order.
ApplyAll(c, from, to, r) ==
    IF from > to THEN r ELSE ApplyAll(c, from + 1, to, ApplyOne(c, from, r))

Numbers(from, to) == [i \in 1 .. (to - from + 1) |-> from + i - 1]

\* The server takes a range first..lst of window c's list (src/sync/actions.cpp, Applier::apply): one that starts past
\* applied + 1 is refused; otherwise the numbers past applied are applied, in order, and a new state is made if the tabs
\* changed. A range wholly applied before changes nothing. (A witness replaces Applier through DeliverWith.)
ApplyRange(c, from, to) ==
    LET r == ApplyAll(c, from, to, [trs |-> tr, qs |-> q, sts |-> stops]) IN
    /\ applied' = [applied EXCEPT ![c] = to]
    /\ done' = [done EXCEPT ![c] = @ \o Numbers(from, to)]
    /\ tr' = r.trs
    /\ q' = r.qs
    /\ stops' = r.sts
    /\ snaps' = Push(r.trs, r.qs, op)
    /\ UNCHANGED <<op, made>>

Applier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, server>>
    ELSE ApplyRange(c, applied[c] + 1, lst)

DeliverWith(Apply(_, _, _), c, first, lst) ==
    /\ 1 <= first /\ first <= lst /\ lst <= Len(acts[c])
    /\ Apply(c, first, lst)
    /\ UNCHANGED <<acts, last, view, vq, seen, live>>

Deliver(c, first, lst) == DeliverWith(Applier, c, first, lst)

\* The server's own steps: the friend's text grows (an open entry is made, or grows by a piece) in a tab it holds.
Grow(t) ==
    /\ made.grows < MaxGrows
    /\ Holds(tr, q, t)
    /\ LET o == IF op[t] = NoOpen THEN [g |-> made.g + 1, len |-> 1] ELSE [op[t] EXCEPT !.len = @ + 1] IN
       /\ op' = [op EXCEPT ![t] = o]
       /\ made' = [made EXCEPT !.g = IF op[t] = NoOpen THEN @ + 1 ELSE @, !.grows = @ + 1]
       /\ snaps' = Push(tr, q, op')
    /\ UNCHANGED <<acts, applied, done, tr, q, stops, last, view, vq, seen, live>>

\* The open entry's block ends: the closed entry goes into the transcript (durable), with all its text.
CloseWith(C(_), t) ==
    /\ op[t] # NoOpen
    /\ tr' = [tr EXCEPT ![t] = Append(@, C(op[t]))]
    /\ op' = [op EXCEPT ![t] = NoOpen]
    /\ snaps' = Push(tr', q, op')
    /\ UNCHANGED <<acts, applied, done, q, stops, made, last, view, vq, seen, live>>
Close(t) == CloseWith(Grown, t)

\* A turn's end with no entry open: the queue's oldest prompt is delivered, as the person's entry.
TakeWith(Tk(_, _), t) ==
    /\ op[t] = NoOpen
    /\ q[t] # <<>>
    /\ LET r == Tk(tr[t], q[t]) IN
       /\ tr' = [tr EXCEPT ![t] = r.tr]
       /\ q' = [q EXCEPT ![t] = r.q]
    /\ snaps' = Push(tr', q', op)
    /\ UNCHANGED <<acts, applied, done, op, stops, made, last, view, vq, seen, live>>
Taken(trt, qt) == [tr |-> Append(trt, Prompt(Head(qt)[1], Head(qt)[2])), q |-> Tail(qt)]
Take(t) == TakeWith(Taken, t)

\* The server restarts from its store: an open entry is lost, and its turn is cut with a notice in the transcript.
RestartWith(Lost(_)) ==
    /\ made.restarts < MaxRestarts
    /\ tr' = [t \in TabNames |-> IF op[t] = NoOpen THEN tr[t] ELSE Lost(t)]
    /\ op' = [t \in TabNames |-> NoOpen]
    /\ made' = [made EXCEPT !.restarts = @ + 1]
    /\ snaps' = Push(tr', q, op')
    /\ UNCHANGED <<acts, applied, done, q, stops, last, view, vq, seen, live>>
CutLost(t) == Append(tr[t], Cut(op[t]))
Restart == RestartWith(CutLost)

\* The window's mirror, given the state s of one tab: a notice for the entries that left the tail before it saw them,
\* then the closed entries it has not shown (an open entry is always the newest, so they stop at it), the open entry
\* held apart, and the queue made the state's.
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

ReceiveWith(Mirror(_, _, _, _), c, j) ==
    /\ last[c] <= j /\ j <= Len(snaps)
    /\ LET m == [t \in TabNames |-> Mirror(view[c][t], vq[c][t], seen[c][t], snaps[j][t])] IN
       /\ view' = [view EXCEPT ![c] = [t \in TabNames |-> m[t].view]]
       /\ vq' = [vq EXCEPT ![c] = [t \in TabNames |-> m[t].vq]]
       /\ seen' = [seen EXCEPT ![c] = [t \in TabNames |-> m[t].seen]]
       /\ live' = [live EXCEPT ![c] = [t \in TabNames |-> m[t].live]]
    /\ last' = [last EXCEPT ![c] = j]
    /\ UNCHANGED <<acts, applied, done, server>>

Receive(c, j) == ReceiveWith(MirrorTab, c, j)

NextWithAll(D(_, _, _), R(_, _), Cl(_), Tk(_), Rs) ==
    \/ \E c \in Clients, k \in Kinds, t \in TabNames : Act(c, k, t)
    \/ \E c \in Clients : \E f, l \in 1 .. Len(acts[c]) : D(c, f, l)
    \/ \E c \in Clients : \E j \in 1 .. Len(snaps) : R(c, j)
    \/ \E t \in TabNames : Grow(t) \/ Cl(t) \/ Tk(t)
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
    /\ \A c \in Clients :
        /\ applied[c] \in 0 .. Len(acts[c])
        /\ last[c] \in 1 .. Len(snaps)
        /\ \A t \in TabNames : seen[c][t].next \in 0 .. Len(tr[t])
    /\ \A t \in TabNames : op[t] = NoOpen \/ (op[t].g \in 1 .. made.g /\ op[t].len >= 1)

\* Up: the server applied window c's actions 1, 2, ..., applied[c], each once, in order, and none it was not given.
ExactlyOnce ==
    \A c \in Clients :
        /\ done[c] = Numbers(1, applied[c])
        /\ applied[c] <= Len(acts[c])

\* Every action applied is in the server's tabs exactly once, where its kind puts it: a send in the transcript; a queue
\* in the queue or, delivered, in the transcript, never both; a stop among the tab's stops, reaching the responder
\* exactly when the server held the tab.
RECURSIVE Count(_, _)
Count(x, s) == IF s = <<>> THEN 0 ELSE (IF Head(s) = x THEN 1 ELSE 0) + Count(x, Tail(s))
StopsOf(t) == [i \in 1 .. Len(stops[t]) |-> stops[t][i].a]
ServerHoldsEach ==
    \A c \in Clients : \A n \in 1 .. Len(acts[c]) :
        LET a == acts[c][n]
            want == IF n <= applied[c] THEN 1 ELSE 0
        IN \A t \in TabNames :
           LET here == IF t = a.tab THEN want ELSE 0 IN
           CASE a.kind = "Send"  -> Count(Prompt(c, n), tr[t]) = here
             [] a.kind = "Queue" -> Count(Prompt(c, n), tr[t]) + Count(<<c, n>>, q[t]) = here
             [] OTHER            -> Count(<<c, n>>, StopsOf(t)) = here

\* Each open entry the server makes ends in the transcript at most once: closed, or cut by a restart, never both.
EachClosedOnce ==
    \A t \in TabNames : \A g \in 1 .. made.g :
        LET closed == {i \in 1 .. Len(tr[t]) : tr[t][i].k \in {"G", "X"} /\ tr[t][i].text[1] = g} IN
        /\ \A i, j \in closed : i = j
        /\ op[t] # NoOpen /\ op[t].g = g => closed = {}

\* A window's transcript with each notice of m entries replaced by m holes: what it shows, by transcript index.
Hole == [k |-> "hole", text |-> <<>>]
RECURSIVE Expand(_)
Expand(v) ==
    IF v = <<>> THEN <<>>
    ELSE LET e == Head(v) IN
         (IF e.k = "N" THEN [i \in 1 .. e.text[1] |-> Hole] ELSE << e >>) \o Expand(Tail(v))

\* Down: each window shows the server's transcript through what it has seen, each entry at its index, or a hole the
\* notice before it accounts for; never an entry twice, never one out of place, never one still open. Its queue is the
\* queue of the state it holds.
ViewMatches ==
    \A c \in Clients : \A t \in TabNames :
        LET x == Expand(view[c][t]) IN
        /\ Len(x) = seen[c][t].next
        /\ \A i \in 1 .. Len(x) : x[i] = Hole \/ x[i] = tr[t][i]
QueueMatches ==
    \A c \in Clients : \A t \in TabNames : vq[c][t] = snaps[last[c]][t].queue
\* No two notices in a row, and none empty: a notice stands for a run of entries the window did not get.
NoticesSound ==
    \A c \in Clients : \A t \in TabNames : \A i \in 1 .. Len(view[c][t]) :
        view[c][t][i].k = "N" => /\ view[c][t][i].text[1] > 0
                                 /\ i > 1 => view[c][t][i - 1].k # "N"

\* The open entry a window holds is text the server had: the open entry still growing, at least as long, or the
\* entry it closed into, or the notice of the restart that lost it. A window never shows text the server never had.
LiveSound ==
    \A c \in Clients : \A t \in TabNames :
        LET e == live[c][t] IN
        e # None =>
            /\ e.k = "O"
            /\ \/ op[t] # NoOpen /\ op[t].g = e.text[1] /\ op[t].len >= e.text[2]
               \/ \E i \in 1 .. Len(tr[t]) : \/ tr[t][i].k = "G" /\ tr[t][i].text[1] = e.text[1]
                                                                /\ tr[t][i].text[2] >= e.text[2]
                                             \/ tr[t][i] = [k |-> "X", text |-> <<e.text[1]>>]

\* The server's open entry only grows, and goes only into the transcript: closed with all its text, or cut.
OpenGrows ==
    [][\A t \in TabNames :
        op[t] # NoOpen =>
            \/ op'[t] # NoOpen /\ op'[t].g = op[t].g /\ op'[t].len >= op[t].len
            \/ op'[t] = NoOpen /\ (tr'[t] = Append(tr[t], Grown(op[t])) \/ tr'[t] = Append(tr[t], Cut(op[t])))]_vars

\* The server's transcripts only grow; its queues only grow at their end, or lose their oldest prompt, one at a time,
\* to the transcript, as the person's entry; every window's transcript only grows, and a window's receiver never goes
\* back.
QueueStep(t) ==
    \/ IsPrefix(q[t], q'[t])
    \/ /\ q[t] # <<>>
       /\ q'[t] = Tail(q[t])
       /\ tr'[t] = Append(tr[t], Prompt(Head(q[t])[1], Head(q[t])[2]))
OnlyGrow ==
    [][/\ \A t \in TabNames : IsPrefix(tr[t], tr'[t]) /\ QueueStep(t) /\ IsPrefix(stops[t], stops'[t])
       /\ \A c \in Clients : /\ \A t \in TabNames : IsPrefix(view[c][t], view'[c][t])
                             /\ last[c] <= last'[c]
                             /\ IsPrefix(done[c], done'[c])]_vars

\* Liveness: once the windows stop acting, the server applies every action and every window shows all of it: every
\* closed entry, the open entry, and the queue.
Settled ==
    \A c \in Clients :
        /\ applied[c] = Len(acts[c])
        /\ \A t \in TabNames : /\ seen[c][t].next = Len(tr[t])
                               /\ vq[c][t] = q[t]
                               /\ live[c][t] = IF op[t] = NoOpen THEN None ELSE Open(op[t])
Converges == <>[]Settled

=============================================================================
