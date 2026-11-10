------------------------------- MODULE Link -------------------------------
(***************************************************************************)
(* What nexus adds on top of mosh's State Synchronization Protocol, between *)
(* the windows and nexus-server (src/sync, src/link, src/server).           *)
(*                                                                         *)
(* Up: each window's actions are an append-only list, numbered from 1      *)
(* (src/sync/actions.h). mosh carries the list as a state; what reaches     *)
(* the server is a range of it, first..last, that may repeat a range it     *)
(* had, come after a later one, or (if mosh ever did) start past what the   *)
(* server has: Deliver takes any range. The server's Applier applies each   *)
(* number once, in order, and refuses a range that would skip one.         *)
(*                                                                         *)
(* Down: the server's tabs (src/sync/hub.h): per tab, the transcript and   *)
(* the queue. What it sends is each transcript's tail, the newest W         *)
(* entries, and the whole queue (src/sync/tabs.h). Every change makes a     *)
(* new state, kept in `snaps` in order. mosh is abstracted to what nexus   *)
(* relies on: a window's receiver eventually holds the newest state; it may *)
(* skip states between; it never goes back to an older one (mosh numbers    *)
(* states and keeps the newest). Receive(c, j) is the window receiving      *)
(* snaps[j], the same one again or a later one, and the window's mirror     *)
(* (src/sync/mirror.h) adding to its own transcript the entries it has not  *)
(* shown, with a notice "N earlier entries not shown" in place of entries   *)
(* that left the tail before it saw them, and to its queue the prompts it   *)
(* has not shown.                                                          *)
(*                                                                         *)
(* Not modelled: mosh itself (its sockets, crypto, fragments, timers,      *)
(* acknowledgements: mosh's own); the hub's limits (a refused action is a   *)
(* notice in the transcript, which the mirror shows like any entry); the    *)
(* tail's byte budget (the tail here is by count only); entries that grow   *)
(* while open (the server has none yet: every entry it makes is closed).   *)
(*                                                                         *)
(* Entries: a prompt is [k |-> "P", text |-> <<c, n>>], action n of window  *)
(* c; its placeholder reply [k |-> "F", text |-> <<>>]; the mirror's notice *)
(* of m entries not shown [k |-> "N", text |-> <<m>>]. A queued prompt is   *)
(* <<c, n>>.                                                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC

CONSTANTS
    Clients,      \* the windows, 1..N
    TabNames,     \* the friends' tabs
    MaxActions,   \* how many actions each window makes, at most
    W             \* the tail: how many of a transcript's newest entries a state carries

ASSUME W >= 1

VARIABLES
    acts,       \* acts[c]: window c's list, each [kind, tab]; action n is acts[c][n]
    applied,    \* applied[c]: the server's Applier for window c: the last number applied
    done,       \* done[c]: every number the server applied for window c, in the order applied (a ghost)
    tr,         \* tr[t]: the server's transcript of tab t
    q,          \* q[t]: the server's queue of tab t
    snaps,      \* the server's states, oldest first; snaps[1] is the empty one it starts with
    last,       \* last[c]: the index in snaps of the state window c's receiver holds
    view,       \* view[c][t]: window c's transcript of tab t
    vq,         \* vq[c][t]: window c's queue of tab t
    seen        \* seen[c][t]: window c's mirror: [next |-> transcript index it shows through, queued |-> queue length]

vars == <<acts, applied, done, tr, q, snaps, last, view, vq, seen>>

Kinds == {"Send", "Queue"}
Max(a, b) == IF a > b THEN a ELSE b
Prompt(c, n) == [k |-> "P", text |-> <<c, n>>]
Reply == [k |-> "F", text |-> <<>>]
Notice(m) == [k |-> "N", text |-> <<m>>]
IsPrefix(a, b) == Len(a) <= Len(b) /\ SubSeq(b, 1, Len(a)) = a

\* What the server sends of its tabs: each transcript's newest W entries, from transcript index `first` (from 0), and
\* the whole queue.
TailFirst(s) == Max(0, Len(s) - W)
Snap(trs, qs) == [t \in TabNames |-> [first   |-> TailFirst(trs[t]),
                                      entries |-> SubSeq(trs[t], TailFirst(trs[t]) + 1, Len(trs[t])),
                                      queue   |-> qs[t]]]

Empty == [t \in TabNames |-> <<>>]

Init ==
    /\ acts = [c \in Clients |-> <<>>]
    /\ applied = [c \in Clients |-> 0]
    /\ done = [c \in Clients |-> <<>>]
    /\ tr = Empty
    /\ q = Empty
    /\ snaps = << Snap(Empty, Empty) >>
    /\ last = [c \in Clients |-> 1]
    /\ view = [c \in Clients |-> Empty]
    /\ vq = [c \in Clients |-> Empty]
    /\ seen = [c \in Clients |-> [t \in TabNames |-> [next |-> 0, queued |-> 0]]]

\* Window c makes an action: the next number in its list.
Act(c, kind, t) ==
    /\ Len(acts[c]) < MaxActions
    /\ acts' = [acts EXCEPT ![c] = Append(@, [kind |-> kind, tab |-> t])]
    /\ UNCHANGED <<applied, done, tr, q, snaps, last, view, vq, seen>>

\* The hub applies action n of window c (src/sync/hub.cpp): a send puts the prompt and its placeholder reply in the
\* transcript, a queue puts the prompt in the queue.
ApplyOne(c, n, trs, qs) ==
    LET a == acts[c][n] IN
    IF a.kind = "Send"
    THEN [trs |-> [trs EXCEPT ![a.tab] = @ \o << Prompt(c, n), Reply >>], qs |-> qs]
    ELSE [trs |-> trs, qs |-> [qs EXCEPT ![a.tab] = Append(@, <<c, n>>)]]

RECURSIVE ApplyAll(_, _, _, _, _)
\* Actions from..to of window c, in order.
ApplyAll(c, from, to, trs, qs) ==
    IF from > to THEN [trs |-> trs, qs |-> qs]
    ELSE LET r == ApplyOne(c, from, trs, qs) IN ApplyAll(c, from + 1, to, r.trs, r.qs)

Numbers(from, to) == [i \in 1 .. (to - from + 1) |-> from + i - 1]

\* The server takes a range first..lst of window c's list (src/sync/actions.cpp, Applier::apply): one that starts past
\* applied + 1 is refused; otherwise the numbers past applied are applied, in order, and a new state is made. A range
\* wholly applied before changes nothing. (A witness replaces Applier through DeliverWith.)
ApplyRange(c, from, to) ==
    LET r == ApplyAll(c, from, to, tr, q) IN
    /\ applied' = [applied EXCEPT ![c] = to]
    /\ done' = [done EXCEPT ![c] = @ \o Numbers(from, to)]
    /\ tr' = r.trs
    /\ q' = r.qs
    /\ snaps' = Append(snaps, Snap(r.trs, r.qs))

Applier(c, first, lst) ==
    IF first > applied[c] + 1 \/ lst <= applied[c]
    THEN UNCHANGED <<applied, done, tr, q, snaps>>
    ELSE ApplyRange(c, applied[c] + 1, lst)

DeliverWith(Apply(_, _, _), c, first, lst) ==
    /\ 1 <= first /\ first <= lst /\ lst <= Len(acts[c])
    /\ Apply(c, first, lst)
    /\ UNCHANGED <<acts, last, view, vq, seen>>

Deliver(c, first, lst) == DeliverWith(Applier, c, first, lst)

\* The window's mirror, given the state s of one tab: a notice for the entries that left the tail before it saw them,
\* then the entries it has not shown, then the queued prompts it has not shown.
MirrorTab(v, qv, se, s) ==
    LET gap == s.first - se.next
        v1 == IF gap > 0 THEN Append(v, Notice(gap)) ELSE v
        from == Max(se.next, s.first)
        total == s.first + Len(s.entries)
        v2 == v1 \o SubSeq(s.entries, from - s.first + 1, Len(s.entries))
        qv2 == qv \o SubSeq(s.queue, se.queued + 1, Len(s.queue))
    IN [view |-> v2, vq |-> qv2, seen |-> [next |-> Max(total, se.next), queued |-> Max(Len(s.queue), se.queued)]]

ReceiveWith(Mirror(_, _, _, _), c, j) ==
    /\ last[c] <= j /\ j <= Len(snaps)
    /\ LET m == [t \in TabNames |-> Mirror(view[c][t], vq[c][t], seen[c][t], snaps[j][t])] IN
       /\ view' = [view EXCEPT ![c] = [t \in TabNames |-> m[t].view]]
       /\ vq' = [vq EXCEPT ![c] = [t \in TabNames |-> m[t].vq]]
       /\ seen' = [seen EXCEPT ![c] = [t \in TabNames |-> m[t].seen]]
    /\ last' = [last EXCEPT ![c] = j]
    /\ UNCHANGED <<acts, applied, done, tr, q, snaps>>

Receive(c, j) == ReceiveWith(MirrorTab, c, j)

NextWith(D(_, _, _), R(_, _)) ==
    \/ \E c \in Clients, k \in Kinds, t \in TabNames : Act(c, k, t)
    \/ \E c \in Clients : \E f, l \in 1 .. Len(acts[c]) : D(c, f, l)
    \/ \E c \in Clients : \E j \in 1 .. Len(snaps) : R(c, j)

Next == NextWith(Deliver, Receive)

\* mosh's promise, the fairness the liveness below needs: what a window has sent reaches the server whole, and the
\* newest state reaches every window.
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
        /\ \A t \in TabNames : seen[c][t].next \in 0 .. Len(tr[t]) /\ seen[c][t].queued \in 0 .. Len(q[t])

\* Up: the server applied window c's actions 1, 2, ..., applied[c], each once, in order, and none it was not given.
ExactlyOnce ==
    \A c \in Clients :
        /\ done[c] = Numbers(1, applied[c])
        /\ applied[c] <= Len(acts[c])

\* Every action applied is in the server's tabs exactly once, where its kind puts it.
RECURSIVE Count(_, _)
Count(x, s) == IF s = <<>> THEN 0 ELSE (IF Head(s) = x THEN 1 ELSE 0) + Count(x, Tail(s))
ServerHoldsEach ==
    \A c \in Clients : \A n \in 1 .. Len(acts[c]) :
        LET a == acts[c][n]
            want == IF n <= applied[c] THEN 1 ELSE 0
        IN IF a.kind = "Send"
           THEN /\ Count(Prompt(c, n), tr[a.tab]) = want
                /\ \A t \in TabNames \ {a.tab} : Count(Prompt(c, n), tr[t]) = 0
           ELSE \A t \in TabNames : Count(<<c, n>>, q[t]) = IF t = a.tab THEN want ELSE 0

\* A window's transcript with each notice of m entries replaced by m holes: what it shows, by transcript index.
Hole == [k |-> "hole", text |-> <<>>]
RECURSIVE Expand(_)
Expand(v) ==
    IF v = <<>> THEN <<>>
    ELSE LET e == Head(v) IN
         (IF e.k = "N" THEN [i \in 1 .. e.text[1] |-> Hole] ELSE << e >>) \o Expand(Tail(v))

\* Down: each window shows the server's transcript through what it has seen, each entry at its index, or a hole the
\* notice before it accounts for; never an entry twice, never one out of place. Its queue is the server's, so far.
ViewMatches ==
    \A c \in Clients : \A t \in TabNames :
        LET x == Expand(view[c][t]) IN
        /\ Len(x) = seen[c][t].next
        /\ \A i \in 1 .. Len(x) : x[i] = Hole \/ x[i] = tr[t][i]
QueueMatches ==
    \A c \in Clients : \A t \in TabNames : vq[c][t] = SubSeq(q[t], 1, seen[c][t].queued)
\* No two notices in a row, and none empty: a notice stands for a run of entries the window did not get.
NoticesSound ==
    \A c \in Clients : \A t \in TabNames : \A i \in 1 .. Len(view[c][t]) :
        view[c][t][i].k = "N" => /\ view[c][t][i].text[1] > 0
                                 /\ i > 1 => view[c][t][i - 1].k # "N"

\* The server's tabs and every window's only grow; a window's receiver never goes back.
OnlyGrow ==
    [][/\ \A t \in TabNames : IsPrefix(tr[t], tr'[t]) /\ IsPrefix(q[t], q'[t])
       /\ \A c \in Clients : /\ \A t \in TabNames : IsPrefix(view[c][t], view'[c][t]) /\ IsPrefix(vq[c][t], vq'[c][t])
                             /\ last[c] <= last'[c]
                             /\ IsPrefix(done[c], done'[c])]_vars

\* Liveness: once the windows stop acting, the server applies every action and every window shows all of it.
Settled ==
    \A c \in Clients :
        /\ applied[c] = Len(acts[c])
        /\ \A t \in TabNames : seen[c][t].next = Len(tr[t]) /\ vq[c][t] = q[t]
Converges == <>[]Settled

=============================================================================
