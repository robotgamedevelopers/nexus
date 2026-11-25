------------------------------ MODULE Steering ------------------------------
(***************************************************************************)
(* Steering a turn that runs: the driver protocol's steer, follow-up,     *)
(* queue and settled (docs/driver-protocol.md, "Steering a turn"), between *)
(* nexus-server's supervisor (src/server/supervisor.h) and one driver      *)
(* (src/driver/steering.h, the driver's side, which the fake driver and    *)
(* any native driver use), over the two pipes, with the harness behind the *)
(* driver.                                                                 *)
(*                                                                         *)
(* The person writes messages, each with an id: a prompt, a steer or a     *)
(* follow-up. The server writes a steer or follow-up into the open turn   *)
(* only to a driver whose hello named that kind (and "settled"), while the *)
(* turn is open, not being stopped and not settled; otherwise the message *)
(* waits as a prompt for the next turn. The driver queues each it takes    *)
(* and reports its whole pending queue (ids, never a diff) on every       *)
(* change. A steer is taken in at the next tool-call boundary, or at the   *)
(* end of a model run; a follow-up only at the end of a model run with no  *)
(* steer pending, one per run. A model run may end and be followed by      *)
(* another (a retry or a compaction, here Retry). When nothing pending and *)
(* nothing automatic remains, the driver writes settled, then done; a stop *)
(* settles at once, aborted, leaving the queue untaken. An id that leaves  *)
(* the driver's queue was taken in by the harness; at done the server puts *)
(* every id it wrote into the turn that was never taken back at the front  *)
(* of its waiting prompts. A driver that exits has its open turn and what  *)
(* it may have half taken cut with a notice; never otherwise.              *)
(*                                                                         *)
(* The server holds at most MaxWaiting messages for the AI (the hub's      *)
(* queue limits, which keep a sync diff small): the prompts waiting and    *)
(* what was written into the open turn and not yet seen taken in, since   *)
(* any of those may wait again at done. A message that would not fit is   *)
(* refused with a notice when the person writes it, never dropped later.  *)
(* The code counts bytes the same way (Supervisor::fits).                  *)
(*                                                                         *)
(* Server state is one record s and driver state one record d, so that the *)
(* server's eager step after each event (Adv: start a driver for a waiting *)
(* prompt, or write the next send to an idle one) is an operator, as the   *)
(* code's Supervisor::advance is.                                          *)
(*                                                                         *)
(* Fault names a planted bug for a reversed witness (none for the spec):   *)
(*   twice          the driver takes steers in at a boundary and keeps them *)
(*   drop           the server drops, at done, what the driver never took  *)
(*   early-follow   the driver takes follow-ups in at a tool boundary      *)
(*   silent         the driver takes messages in without reporting the queue*)
(*   unnamed        the server steers a driver that did not name the kind  *)
(*   stale          the driver takes a steer of a turn that is not open    *)
(*   settle-early   the driver settles with follow-ups pending             *)
(*   send-at-settled the server closes the turn at settled, not at done    *)
(*   loop           the driver never takes a follow-up in, nor settles     *)
(*   unbounded      the server writes into the turn past the waiting limit *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS MaxIds,       \* messages the person writes, in all
          MaxStarts,    \* driver processes, in all
          MaxTools,     \* tool rounds in one turn
          MaxRetries,   \* model runs after the first that are not steering's (retry, compaction)
          MaxWaiting,   \* messages the server holds for the AI: waiting, and written into the turn untaken
          FeatureSets,  \* the features a driver's hello may name
          Fault

Kinds == {"steer", "follow"}

ASSUME FeatureSets \subseteq SUBSET (Kinds \cup {"settled"})
ASSUME \A f \in FeatureSets : (f \cap Kinds # {}) => "settled" \in f

VARIABLES feats,    \* what the hello named (every driver of the run is the same program)
          kinds,    \* kinds[id]: "prompt", "steer" or "follow", as the person wrote it
          s,        \* the server's supervisor
          d,        \* the driver
          s2d,      \* server to driver, a FIFO of lines
          d2s,      \* driver to server
          hlog      \* the harness: each id it took in, in order

vars == <<feats, kinds, s, d, s2d, d2s, hlog>>

Range(q) == {q[i] : i \in 1..Len(q)}

Msg(k, t, i) == [k |-> k, t |-> t, i |-> i, s |-> <<>>, f |-> <<>>, a |-> FALSE]
Flag(k, t, a) == [k |-> k, t |-> t, i |-> 0, s |-> <<>>, f |-> <<>>, a |-> a]
QueueMsg(t, ps, pf) == [k |-> "queue", t |-> t, i |-> 0, s |-> ps, f |-> pf, a |-> FALSE]
Hello == Flag("hello", 0, FALSE)

NoRep == [steer |-> <<>>, follow |-> <<>>]

ServerInit == [drv |-> "none", starts |-> 0, turn |-> 0, next |-> 1, prompt |-> 0, stopping |-> FALSE,
               settled |-> FALSE, waiting |-> <<>>, inflight |-> <<>>, rep |-> NoRep, ever |-> {}, cut |-> {},
               refused |-> {}, broken |-> FALSE]

DriverInit == [alive |-> FALSE, turn |-> 0, last |-> 0, lastid |-> 0, ps |-> <<>>, pf |-> <<>>, run |-> "idle",
               tools |-> 0, retries |-> 0, aborted |-> FALSE, broken |-> FALSE]
Fresh == [DriverInit EXCEPT !.alive = TRUE]

Init == /\ feats \in FeatureSets
        /\ kinds = <<>>
        /\ s = ServerInit
        /\ d = DriverInit
        /\ s2d = <<>>
        /\ d2s = <<>>
        /\ hlog = <<>>

(* ---- the server ---- *)

\* The driver took id in: the server saw it in a report and saw it leave.
Taken(st, id) == id \in st.ever /\ id \notin Range(st.rep.steer) \cup Range(st.rep.follow)

\* After each server event: a driver started for a waiting prompt, or the oldest waiting prompt written to an idle one
\* as the next turn. Returns the server, the driver and both pipes.
Adv(st, dv, out, back) ==
    IF st.drv = "none" /\ st.waiting # <<>>
    THEN [s |-> [st EXCEPT !.drv = "starting", !.starts = @ + 1], d |-> Fresh, c |-> <<>>, b |-> <<Hello>>]
    ELSE IF st.drv = "up" /\ st.turn = 0 /\ st.waiting # <<>>
    THEN [s |-> [st EXCEPT !.turn = st.next, !.next = @ + 1, !.prompt = Head(st.waiting), !.waiting = Tail(@),
                           !.stopping = FALSE, !.settled = FALSE, !.inflight = <<>>, !.rep = NoRep, !.ever = {}],
          d |-> dv, c |-> Append(out, Msg("send", st.next, Head(st.waiting))), b |-> back]
    ELSE [s |-> st, d |-> dv, c |-> out, b |-> back]

Set(r) == s' = r.s /\ d' = r.d /\ s2d' = r.c /\ d2s' = r.b

\* The turn closes: what the driver never took in waits again, first, in the order written.
Close(st) ==
    LET back == IF Fault = "drop" THEN <<>> ELSE SelectSeq(st.inflight, LAMBDA id : ~Taken(st, id))
    IN [st EXCEPT !.turn = 0, !.prompt = 0, !.waiting = back \o @, !.inflight = <<>>, !.rep = NoRep, !.ever = {},
                  !.settled = FALSE, !.stopping = FALSE]

NewId == Len(kinds) + 1

\* What the server holds for the AI that may wait at the next done: the prompts waiting, and what it wrote into the
\* open turn and has not seen taken in.
Held(st) == Len(st.waiting) + Cardinality({id \in Range(st.inflight) : ~Taken(st, id)})
Fits == Held(s) < MaxWaiting

\* The person's message waits as a prompt, or is refused with a notice when it would not fit.
Wait == IF Fits THEN Set(Adv([s EXCEPT !.waiting = Append(@, NewId)], d, s2d, d2s))
        ELSE s' = [s EXCEPT !.refused = @ \cup {NewId}] /\ UNCHANGED <<d, s2d, d2s>>

Prompt == /\ Len(kinds) < MaxIds
          /\ kinds' = Append(kinds, "prompt")
          /\ Wait
          /\ UNCHANGED <<feats, hlog>>

CanSteer(k) == /\ s.drv = "up" /\ s.turn # 0 /\ ~s.stopping /\ ~s.settled
               /\ (k \in feats \/ Fault = "unnamed")

Steer(k) == /\ Len(kinds) < MaxIds
            /\ kinds' = Append(kinds, k)
            /\ IF CanSteer(k) /\ (Fits \/ Fault = "unbounded")
               THEN /\ s' = [s EXCEPT !.inflight = Append(@, NewId)]
                    /\ s2d' = Append(s2d, Msg(k, s.turn, NewId))
                    /\ UNCHANGED <<d, d2s>>
               ELSE Wait
            /\ UNCHANGED <<feats, hlog>>

Stop == /\ s.drv = "up" /\ s.turn # 0 /\ ~s.stopping
        /\ s' = [s EXCEPT !.stopping = TRUE]
        /\ s2d' = Append(s2d, Msg("stop", s.turn, 0))
        /\ UNCHANGED <<feats, kinds, d, d2s, hlog>>

\* A queue report is whole and sound: of the open turn, before settled, from a driver that named a kind; each id one
\* the server wrote into this turn as that kind, in the order written, and none the driver already took in.
SoundQueue(m) ==
    LET ok(q, k) == /\ \A i \in 1..Len(q) : q[i] \in Range(s.inflight) /\ kinds[q[i]] = k /\ ~Taken(s, q[i])
                    /\ \A i, j \in 1..Len(q) : i < j => q[i] < q[j]
    IN /\ m.t = s.turn /\ s.turn # 0 /\ ~s.settled /\ feats \cap Kinds # {}
       /\ ok(m.s, "steer") /\ ok(m.f, "follow")

Broken == [s EXCEPT !.broken = TRUE]

Receive == /\ d2s # <<>>
           /\ LET m == Head(d2s) IN
              CASE m.k = "hello" ->
                     Set(Adv([s EXCEPT !.drv = "up"], d, s2d, Tail(d2s)))
                [] m.k = "queue" ->
                     /\ s' = IF SoundQueue(m)
                             THEN [s EXCEPT !.rep = [steer |-> m.s, follow |-> m.f],
                                            !.ever = @ \cup Range(m.s) \cup Range(m.f)]
                             ELSE Broken
                     /\ d2s' = Tail(d2s) /\ UNCHANGED <<d, s2d>>
                [] m.k = "settled" /\ Fault = "send-at-settled" ->
                     Set(Adv(Close(s), d, s2d, Tail(d2s)))
                [] m.k = "settled" ->
                     /\ s' = IF m.t = s.turn /\ s.turn # 0 /\ ~s.settled /\ "settled" \in feats
                             THEN [s EXCEPT !.settled = TRUE] ELSE Broken
                     /\ d2s' = Tail(d2s) /\ UNCHANGED <<d, s2d>>
                [] m.k = "done" ->
                     IF m.t = s.turn /\ s.turn # 0 /\ ("settled" \in feats => s.settled)
                     THEN Set(Adv(Close(s), d, s2d, Tail(d2s)))
                     ELSE IF Fault = "send-at-settled"
                     THEN Set([s |-> s, d |-> d, c |-> s2d, b |-> Tail(d2s)])
                     ELSE Set([s |-> Broken, d |-> d, c |-> s2d, b |-> Tail(d2s)])
           /\ UNCHANGED <<feats, kinds, hlog>>

\* The driver's exit, seen once everything it wrote is read: the open turn is cut, and with it what the driver may have
\* half taken; the waiting prompts stay, and a driver starts again for them.
Exited == /\ s.drv # "none" /\ ~d.alive /\ d2s = <<>>
          /\ LET lost == {id \in Range(s.inflight) : ~Taken(s, id)} \cup (IF s.turn # 0 THEN {s.prompt} ELSE {})
                 st == [s EXCEPT !.drv = "none", !.turn = 0, !.prompt = 0, !.inflight = <<>>, !.rep = NoRep,
                                 !.ever = {}, !.settled = FALSE, !.stopping = FALSE, !.cut = @ \cup lost]
             IN Set(Adv(st, d, <<>>, <<>>))
          /\ UNCHANGED <<feats, kinds, hlog>>

(* ---- the driver ---- *)

Refuse == [d EXCEPT !.broken = TRUE]

Settles(dv) == IF "settled" \in feats THEN <<Flag("settled", dv.turn, dv.aborted)>> ELSE <<>>

Take(m) ==
    CASE m.k = "send" ->
           IF d.turn # 0 \/ m.t <= d.last
           THEN d' = Refuse /\ UNCHANGED <<d2s, hlog>>
           ELSE /\ d' = [d EXCEPT !.turn = m.t, !.last = m.t, !.run = "model", !.tools = 0, !.retries = 0,
                                  !.aborted = FALSE]
                /\ hlog' = Append(hlog, m.i)
                /\ UNCHANGED d2s
      [] m.k \in Kinds ->
           IF m.k \notin feats \/ m.t > d.last \/ m.i <= d.lastid
           THEN d' = Refuse /\ UNCHANGED <<d2s, hlog>>
           ELSE IF m.t = d.turn /\ d.run \in {"model", "tools"}
           THEN LET ps == IF m.k = "steer" THEN Append(d.ps, m.i) ELSE d.ps
                    pf == IF m.k = "follow" THEN Append(d.pf, m.i) ELSE d.pf
                IN /\ d' = [d EXCEPT !.lastid = m.i, !.ps = ps, !.pf = pf]
                   /\ d2s' = Append(d2s, QueueMsg(d.turn, ps, pf))
                   /\ UNCHANGED hlog
           ELSE IF Fault = "stale" /\ m.k = "steer"
           THEN d' = [d EXCEPT !.lastid = m.i, !.ps = Append(@, m.i)] /\ UNCHANGED <<d2s, hlog>>
           ELSE d' = [d EXCEPT !.lastid = m.i] /\ UNCHANGED <<d2s, hlog>>
      [] m.k = "stop" ->
           IF m.t = d.turn /\ d.turn # 0 /\ d.run \in {"model", "tools"}
           THEN LET dv == [d EXCEPT !.run = "settling", !.aborted = TRUE]
                IN d' = dv /\ d2s' = d2s \o Settles(dv) /\ UNCHANGED hlog
           ELSE UNCHANGED <<d, d2s, hlog>>

Read == /\ d.alive /\ s2d # <<>>
        /\ s2d' = Tail(s2d)
        /\ Take(Head(s2d))
        /\ UNCHANGED <<feats, kinds, s>>

\* The model's call ends with tool calls to run.
Tools == /\ d.alive /\ d.run = "model" /\ d.tools < MaxTools
         /\ d' = [d EXCEPT !.run = "tools", !.tools = @ + 1]
         /\ UNCHANGED <<feats, kinds, s, s2d, d2s, hlog>>

\* The tool calls are done, before the next model call: the steers pending go in, all of them.
Boundary == /\ d.alive /\ d.run = "tools"
            /\ LET took == IF Fault = "early-follow" THEN d.ps \o d.pf ELSE d.ps
                   ps == IF Fault = "twice" THEN d.ps ELSE <<>>
                   pf == IF Fault = "early-follow" THEN <<>> ELSE d.pf
               IN /\ hlog' = hlog \o took
                  /\ d' = [d EXCEPT !.run = "model", !.ps = ps, !.pf = pf]
                  /\ d2s' = IF took # <<>> /\ Fault # "silent" THEN Append(d2s, QueueMsg(d.turn, ps, pf)) ELSE d2s
            /\ UNCHANGED <<feats, kinds, s, s2d>>

\* A model run ends with no tool calls: steers pending go in and the run goes on; else the oldest follow-up goes in.
RunEnd == /\ d.alive /\ d.run = "model"
          /\ \/ /\ d.ps # <<>>
                /\ hlog' = hlog \o d.ps
                /\ d' = [d EXCEPT !.ps = <<>>]
                /\ d2s' = IF Fault # "silent" THEN Append(d2s, QueueMsg(d.turn, <<>>, d.pf)) ELSE d2s
             \/ /\ d.ps = <<>> /\ d.pf # <<>> /\ Fault # "loop"
                /\ hlog' = Append(hlog, Head(d.pf))
                /\ d' = [d EXCEPT !.pf = Tail(@)]
                /\ d2s' = IF Fault # "silent" THEN Append(d2s, QueueMsg(d.turn, <<>>, Tail(d.pf))) ELSE d2s
          /\ UNCHANGED <<feats, kinds, s, s2d>>

\* A model run ends and another follows without the person: a retry, or a compaction and its retry.
Retry == /\ d.alive /\ d.run = "model" /\ d.retries < MaxRetries
         /\ d' = [d EXCEPT !.retries = @ + 1]
         /\ UNCHANGED <<feats, kinds, s, s2d, d2s, hlog>>

\* Nothing pending and nothing automatic left: settled (to a driver that named it), then done.
Settle == /\ d.alive /\ d.run = "model" /\ d.ps = <<>>
          /\ (d.pf = <<>> \/ Fault = "settle-early")
          /\ LET dv == [d EXCEPT !.run = "settling"] IN d' = dv /\ d2s' = d2s \o Settles(dv)
          /\ UNCHANGED <<feats, kinds, s, s2d, hlog>>

Finish == /\ d.alive /\ d.run = "settling"
          /\ d2s' = Append(d2s, Flag("done", d.turn, d.aborted))
          /\ d' = [d EXCEPT !.run = "idle", !.turn = 0, !.ps = <<>>, !.pf = <<>>]
          /\ UNCHANGED <<feats, kinds, s, s2d, hlog>>

\* The driver ends, at any time: a crash, or its harness gone. A next one can always start (the starts' bound).
Exit == /\ d.alive /\ s.starts < MaxStarts
        /\ d' = [d EXCEPT !.alive = FALSE]
        /\ UNCHANGED <<feats, kinds, s, s2d, d2s, hlog>>

Next == \/ Prompt \/ Stop \/ \E k \in Kinds : Steer(k)
        \/ Receive \/ Exited
        \/ Read \/ Tools \/ Boundary \/ RunEnd \/ Retry \/ Settle \/ Finish \/ Exit

\* The person, the model's tool calls, its retries and the driver's exit are not owed; the rest is.
Fairness == /\ WF_vars(Receive) /\ WF_vars(Exited) /\ WF_vars(Read)
            /\ WF_vars(Boundary) /\ WF_vars(RunEnd) /\ WF_vars(Settle) /\ WF_vars(Finish)

Spec == Init /\ [][Next]_vars /\ Fairness

(* ---- the properties ---- *)

\* Each message reaches the harness at most once: as a send, a steer or a follow-up, never two of them.
AtMostOnce == \A i, j \in 1..Len(hlog) : i # j => hlog[i] # hlog[j]

\* Each message the person wrote is in the harness, waiting, written into the open turn, the open turn's prompt, cut
\* with a notice, or refused with a notice when written.
NeverLost == \A id \in 1..Len(kinds) :
    \/ id \in Range(hlog) \/ id \in Range(s.waiting) \/ id \in Range(s.inflight) \/ id \in s.cut \/ id \in s.refused
    \/ (id = s.prompt /\ s.turn # 0)

\* The waiting prompts never pass the limit, at done (what is given back) as at any other time.
WaitingFits == Len(s.waiting) <= MaxWaiting

\* Cut only when the driver is gone: never lost while it lives.
CutOnlyOnExit == [][s'.cut # s.cut => ~d.alive]_vars

\* A follow-up written into a turn goes in only at the end of a model run, with no steer pending (one that waited as a
\* prompt goes in as its turn's send, with the driver idle).
FollowAtRunEnd == [][\A j \in Len(hlog) + 1..Len(hlog') :
                        (kinds[hlog'[j]] = "follow" /\ d.run # "idle") => d.run = "model" /\ d.ps = <<>>]_vars

\* A settle that is not a stop leaves nothing pending behind it.
SettleEmpty == [][(d.alive /\ d'.alive /\ d.run # "settling" /\ d'.run = "settling" /\ ~d'.aborted)
                    => d.ps = <<>> /\ d.pf = <<>>]_vars

\* Once the server has read every report, its queue is the driver's.
ViewMatches == (d.alive /\ d.turn # 0 /\ d.turn = s.turn /\ \A j \in 1..Len(d2s) : d2s[j].k # "queue")
                   => s.rep = [steer |-> d.ps, follow |-> d.pf]

\* A steer or follow-up is written only to a driver that named its kind.
NamedOnly == \A j \in 1..Len(s2d) : s2d[j].k \in Kinds => s2d[j].k \in feats

\* Neither side breaks a rule the other refuses: no send while a turn is open or unsettled, no kind unnamed, no turn
\* out of order, no unsound report, no done before settled.
DriverKeptRules == ~d.broken
ServerKeptRules == ~s.broken

Safety == AtMostOnce /\ NeverLost /\ WaitingFits /\ ViewMatches /\ NamedOnly /\ DriverKeptRules /\ ServerKeptRules

\* Every turn ends, and every message written into a turn is taken in, given back or cut.
TurnsEnd == \A t \in 1..MaxIds : (s.turn = t) ~> (s.turn # t)
Resolved == \A id \in 1..MaxIds : (id \in Range(s.inflight)) ~> (id \notin Range(s.inflight))
=============================================================================
