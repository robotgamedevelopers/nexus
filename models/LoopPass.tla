------------------------------ MODULE LoopPass ------------------------------
(***************************************************************************)
(* One pass of nexus-server's serving loop, repeated across passes,        *)
(* batches of client actions and driver messages, store failures and       *)
(* crashes at any point (src/server/pass.h PassOrder and take_pass_step,   *)
(* src/server/server.cpp Server::run, src/server/drivers.cpp Drivers::hold *)
(* and release, src/server/durable.cpp DurableStore::beginBatch and        *)
(* flush).                                                                 *)
(*                                                                         *)
(* Each pass of the loop runs in Order:                                    *)
(*   "tick"     every link ticks at the top of the pass: the state set at  *)
(*              the end of the previous pass leaves on the wire            *)
(*              (sentSnap), and mosh acknowledges the client actions       *)
(*              received before that flush (acked)                         *)
(*   "hold"     DurableStore::beginBatch and Drivers::hold: commits append *)
(*              to the batch in memory without syncing, and commands told  *)
(*              to a driver (writes of sends and stops) wait in heldCmds   *)
(*   "apply"    zero or more batch steps while held: a driver's hello or   *)
(*              done (Drivers::serve), and client actions received and     *)
(*              applied in order (ServerSession::receive, take, Hub::apply *)
(*              through Drivers); an earlier store fault fails the pass    *)
(*   "flush"    DurableStore::flush: one sync makes every record of the    *)
(*              batch durable (disk); a failed flush ends the pass failed  *)
(*   "release"  Drivers::release: held commands go out to the live driver  *)
(*              in the order they were told                                *)
(*   "state"    when the pass changed anything, ServerSession::setState    *)
(*              stages the new state (linkSnap) for the next pass's tick   *)
(*                                                                         *)
(* A failed apply or flush stops the loop (failed = TRUE): neither         *)
(* release, state nor another tick runs.                                   *)
(*                                                                         *)
(* Crash(k) may happen at any point, keeping any prefix 0..Len(batch) of   *)
(* the current batch's records on disk (0 before the flush's write or on a *)
(* reverted write; 0..Len(batch) when a crash tears the batch's write      *)
(* before sync returns, as models/store/Journal.tla and MemFs allow).      *)
(* Recovery reopens the store from what survived on disk, cuts any open    *)
(* turn with a notice (never resending its prompt), takes the oldest       *)
(* queued prompt if nothing waits, and lets each window reconnect from     *)
(* acked[c] + 1 with its Applier initialized to disk.applied[c].           *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Clients,        \* the connected windows, 1..N
    MaxActs,        \* at most how many actions each window makes
    Kinds,          \* subset of {"Send", "Queue", "Stop"}
    MaxCrashes,     \* at most how many crashes / restarts happen
    MaxFails        \* at most how many store / flush failures happen

ASSUME /\ Kinds \subseteq {"Send", "Queue", "Stop"}
       /\ MaxActs \in Nat /\ MaxCrashes \in Nat /\ MaxFails \in Nat

\* Default pass configuration; witnesses override one part through NextWith.
DefaultOrder == <<"tick", "hold", "apply", "flush", "release", "state">>

VARIABLES
    stepIdx,        \* 1..6: index in the pass's Order
    failed,         \* TRUE once a store commit or flush failed in this run (the loop stops)
    batching,       \* Store::batching(): TRUE from Hold until Flush
    holding,        \* Drivers::holding(): TRUE from Hold until Release
    changed,        \* whether this pass's Apply changed session or driver state
    acts,           \* acts[c]: window c's action kinds, 1..Len(acts[c])
    rcvd,           \* rcvd[c]: highest action number the server's link receiver holds for window c
    applier,        \* applier[c]: the server's Applier counter for window c's current link
    acked,          \* acked[c]: highest action number acknowledged to window c by a link tick
    mem,            \* in-memory StoreState of session 1 and client counters
    disk,           \* durable StoreState on disk
    batch,          \* sequence of uncommitted StoreStates appended in the current batch
    drvPhase,       \* live driver's supervisor phase: "none", "starting", "idle", "working", "stopping"
    drvTurn,        \* turn number actually written to the live driver (0 when none yet)
    heldCmds,       \* commands queued while holding: [kind |-> "send"|"stop", ...]
    drvSends,       \* history of sends actually written to drivers: <<[turn |-> t, prompt |-> p], ...>>
    drvStops,       \* history of stops actually written to drivers: <<[turn |-> t, by |-> <<c, n>>], ...>>
    linkSnap,       \* state staged on the links by setState (and at connect/recover)
    sentSnap,       \* newest state transmitted to windows by Tick
    crashes,        \* crashes so far
    fails           \* store/flush failures so far

vars == <<stepIdx, failed, batching, holding, changed, acts, rcvd, applier, acked,
          mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
          linkSnap, sentSnap, crashes, fails>>

\* Model view for state comparison (crashes and fails are bounded counters part of the state).
Max(a, b) == IF a > b THEN a ELSE b
IsPrefix(a, b) == Len(a) <= Len(b) /\ SubSeq(b, 1, Len(a)) = a
Numbers(n) == [i \in 1..n |-> i]

NoPrompt == <<0, 0>>
EntryP(p) == [k |-> "P", t |-> 0, p |-> p]
EntryF(t) == [k |-> "F", t |-> t, p |-> NoPrompt]
EntryK    == [k |-> "K", t |-> 0, p |-> NoPrompt]
EntryX    == [k |-> "X", t |-> 0, p |-> NoPrompt]

SendCmd(t, p) == [kind |-> "send", turn |-> t, prompt |-> p, by |-> NoPrompt]
StopCmd(t, who) == [kind |-> "stop", turn |-> t, prompt |-> NoPrompt, by |-> who]

Snap(st) == [tr |-> st.tr, queue |-> st.queue]

EmptyStore ==
    [tr      |-> <<>>,
     queue   |-> <<>>,
     waiting |-> <<>>,
     open    |-> 0,
     next    |-> 1,
     applied |-> [c \in Clients |-> 0],
     hist    |-> [c \in Clients |-> <<>>]]

Init ==
    /\ stepIdx = 1
    /\ failed = FALSE
    /\ batching = FALSE
    /\ holding = FALSE
    /\ changed = FALSE
    /\ acts = [c \in Clients |-> <<>>]
    /\ rcvd = [c \in Clients |-> 0]
    /\ applier = [c \in Clients |-> 0]
    /\ acked = [c \in Clients |-> 0]
    /\ mem = EmptyStore
    /\ disk = EmptyStore
    /\ batch = <<>>
    /\ drvPhase = "none"
    /\ drvTurn = 0
    /\ heldCmds = <<>>
    /\ drvSends = <<>>
    /\ drvStops = <<>>
    /\ linkSnap = Snap(EmptyStore)
    /\ sentSnap = Snap(EmptyStore)
    /\ crashes = 0
    /\ fails = 0

----------------------------------------------------------------------------
\* Supervisor + Drivers step transitions on (st, ph), matching src/server/supervisor.cpp
\* and src/server/drivers.cpp.

\* Supervisor::take: when no turn is open and nothing waits, the oldest queued prompt joins waiting.
TakeStep(st) ==
    IF st.open = 0 /\ st.waiting = <<>> /\ st.queue # <<>>
    THEN LET p == Head(st.queue) IN
         [st EXCEPT !.tr = Append(@, EntryP(p)), !.queue = Tail(@), !.waiting = <<p>>]
    ELSE st

\* Supervisor::advance on (st, ph): take, startDriver if none, writeNext if idle.
\* Returns [st, ph, cmds].
Advance(st, ph) ==
    LET st1 == TakeStep(st) IN
    IF ph = "none" /\ st1.waiting # <<>>
    THEN [st |-> st1, ph |-> "starting", cmds |-> <<>>]
    ELSE IF ph = "idle" /\ st1.waiting # <<>>
    THEN LET p == Head(st1.waiting)
             t == st1.next
             st2 == [st1 EXCEPT !.waiting = Tail(@), !.open = t, !.next = t + 1]
         IN [st |-> st2, ph |-> "working", cmds |-> <<SendCmd(t, p)>>]
    ELSE [st |-> st1, ph |-> ph, cmds |-> <<>>]

\* Delivering commands cmds: held in heldCmds while holding = TRUE, carried out immediately otherwise.
RECURSIVE DeliverNow(_, _, _, _)
DeliverNow(cmds, sends, stops, dturn) ==
    IF cmds = <<>>
    THEN [sends |-> sends, stops |-> stops, dturn |-> dturn]
    ELSE LET c == Head(cmds)
             sends1 == IF c.kind = "send"
                       THEN Append(sends, [turn |-> c.turn, prompt |-> c.prompt])
                       ELSE sends
             stops1 == IF c.kind = "stop"
                       THEN Append(stops, [turn |-> c.turn, by |-> c.by])
                       ELSE stops
             dturn1 == IF c.kind = "send" THEN c.turn ELSE dturn
         IN DeliverNow(Tail(cmds), sends1, stops1, dturn1)

EmitCmds(cmds) ==
    IF cmds = <<>>
    THEN UNCHANGED <<heldCmds, drvSends, drvStops, drvTurn>>
    ELSE IF holding
    THEN /\ heldCmds' = heldCmds \o cmds
         /\ UNCHANGED <<drvSends, drvStops, drvTurn>>
    ELSE LET d == DeliverNow(cmds, drvSends, drvStops, drvTurn) IN
         /\ heldCmds' = heldCmds
         /\ drvSends' = d.sends
         /\ drvStops' = d.stops
         /\ drvTurn' = d.dturn

NextIdx(ord) == IF stepIdx = Len(ord) THEN 1 ELSE stepIdx + 1

----------------------------------------------------------------------------
\* Window actions (environment: may happen at any time up to MaxActs).

Act(c, k) ==
    /\ c \in Clients /\ k \in Kinds
    /\ Len(acts[c]) < MaxActs
    /\ acts' = [acts EXCEPT ![c] = Append(@, k)]
    /\ UNCHANGED <<stepIdx, failed, batching, holding, changed, rcvd, applier, acked,
                   mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   linkSnap, sentSnap, crashes, fails>>

----------------------------------------------------------------------------
\* Pass steps, driven in the order `ord` names.

\* Top of pass: a fresh PassOrder starts (changed = FALSE when stepIdx = 1), and links tick,
\* sending linkSnap and acknowledging received actions.
Tick(ord) ==
    /\ ~failed /\ ord[stepIdx] = "tick"
    /\ stepIdx' = NextIdx(ord)
    /\ changed' = IF stepIdx = 1 THEN FALSE ELSE changed
    /\ acked' = [c \in Clients |-> Max(acked[c], rcvd[c])]
    /\ sentSnap' = linkSnap
    /\ UNCHANGED <<failed, batching, holding, acts, rcvd, applier,
                   mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   linkSnap, crashes, fails>>

\* Start of batch: store_.beginBatch() and (when holdDrv = TRUE) drivers_->hold().
Hold(ord, holdDrv) ==
    /\ ~failed /\ ord[stepIdx] = "hold"
    /\ stepIdx' = NextIdx(ord)
    /\ batching' = TRUE
    /\ holding' = holdDrv
    /\ changed' = FALSE
    /\ batch' = <<>>
    /\ UNCHANGED <<failed, acts, rcvd, applier, acked, mem, disk, drvPhase, drvTurn,
                   heldCmds, drvSends, drvStops, linkSnap, sentSnap, crashes, fails>>

\* In Apply: a spawned driver says hello + credential (Drivers::serve).
DriverHello(ord) ==
    /\ ~failed /\ ord[stepIdx] = "apply"
    /\ drvPhase = "starting"
    /\ LET res == Advance(mem, "idle")
           committed == (res.st # mem)
       IN /\ mem' = res.st
          /\ batch' = IF committed THEN Append(batch, res.st) ELSE batch
          /\ drvPhase' = res.ph
          /\ changed' = TRUE
          /\ EmitCmds(res.cmds)
    /\ UNCHANGED <<stepIdx, failed, batching, holding, acts, rcvd, applier, acked,
                   disk, linkSnap, sentSnap, crashes, fails>>

\* In Apply: the live driver finishes the open turn that was written to it (Drivers::serve).
DriverDone(ord) ==
    /\ ~failed /\ ord[stepIdx] = "apply"
    /\ drvPhase \in {"working", "stopping"}
    /\ mem.open # 0 /\ drvTurn = mem.open
    /\ LET entry == IF drvPhase = "working" THEN EntryF(mem.open) ELSE EntryK
           st0   == [mem EXCEPT !.tr = Append(@, entry), !.open = 0]
           res   == Advance(st0, "idle")
           dBase == DeliverNow(res.cmds, drvSends, drvStops, 0)
       IN /\ mem' = res.st
          /\ batch' = Append(batch, res.st)
          /\ drvPhase' = res.ph
          /\ changed' = TRUE
          /\ IF holding
             THEN /\ heldCmds' = heldCmds \o res.cmds
                  /\ drvTurn' = 0
                  /\ UNCHANGED <<drvSends, drvStops>>
             ELSE /\ heldCmds' = heldCmds
                  /\ drvSends' = dBase.sends
                  /\ drvStops' = dBase.stops
                  /\ drvTurn' = dBase.dturn
    /\ UNCHANGED <<stepIdx, failed, batching, holding, acts, rcvd, applier, acked,
                   disk, linkSnap, sentSnap, crashes, fails>>

\* In Apply: link c receives a packet carrying actions already applied (e.g. resent after a crash
\* between Flush and Tick), advancing rcvd[c] without re-applying them.
RecvOnly(ord, c, to) ==
    /\ ~failed /\ ord[stepIdx] = "apply"
    /\ c \in Clients
    /\ rcvd[c] < to /\ to <= applier[c] /\ to <= Len(acts[c])
    /\ rcvd' = [rcvd EXCEPT ![c] = to]
    /\ UNCHANGED <<stepIdx, failed, batching, holding, changed, acts, applier, acked,
                   mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   linkSnap, sentSnap, crashes, fails>>

\* One client action (c, n) applied to (st, ph): returns [st, ph, cmds].
ApplyClientAction(st, ph, c, n) ==
    LET k   == acts[c][n]
        p   == <<c, n>>
        stA == [st EXCEPT !.applied[c] = n, !.hist[c] = Append(@, n)]
    IN
    CASE k = "Send" ->
            LET pre  == Advance(stA, ph)
                post == Advance([pre.st EXCEPT !.tr = Append(@, EntryP(p)), !.waiting = Append(@, p)], pre.ph)
            IN [st |-> post.st, ph |-> post.ph, cmds |-> pre.cmds \o post.cmds]
      [] k = "Queue" ->
            Advance([stA EXCEPT !.queue = Append(@, p)], ph)
      [] OTHER -> \* "Stop"
            IF ph = "working"
            THEN [st |-> stA, ph |-> "stopping", cmds |-> <<StopCmd(stA.open, p)>>]
            ELSE IF ph = "starting" /\ stA.waiting # <<>>
            THEN [st |-> [stA EXCEPT !.waiting = Tail(@), !.tr = Append(@, EntryK)],
                  ph |-> "starting", cmds |-> <<>>]
            ELSE [st |-> stA, ph |-> ph, cmds |-> <<>>]

RECURSIVE ApplyRangeStep(_, _, _, _, _, _, _, _, _, _)
ApplyRangeStep(st, ph, b, h, sends, stops, dturn, c, from, to) ==
    IF from > to
    THEN [st |-> st, ph |-> ph, batch |-> b, held |-> h,
          sends |-> sends, stops |-> stops, dturn |-> dturn]
    ELSE LET res == ApplyClientAction(st, ph, c, from)
             h1  == IF holding THEN h \o res.cmds ELSE h
             d1  == IF holding
                    THEN [sends |-> sends, stops |-> stops, dturn |-> dturn]
                    ELSE DeliverNow(res.cmds, sends, stops, dturn)
         IN ApplyRangeStep(res.st, res.ph, Append(b, res.st), h1,
                           d1.sends, d1.stops, d1.dturn, c, from + 1, to)

\* In Apply: link c receives and applies its unapplied actions applier[c] + 1 .. to in order.
ApplyAct(ord, c, to) ==
    /\ ~failed /\ ord[stepIdx] = "apply"
    /\ c \in Clients
    /\ applier[c] < to /\ to <= Len(acts[c])
    /\ LET r == ApplyRangeStep(mem, drvPhase, batch, heldCmds,
                               drvSends, drvStops, drvTurn, c, applier[c] + 1, to)
       IN /\ rcvd' = [rcvd EXCEPT ![c] = Max(rcvd[c], to)]
          /\ applier' = [applier EXCEPT ![c] = to]
          /\ mem' = r.st
          /\ batch' = r.batch
          /\ drvPhase' = r.ph
          /\ changed' = TRUE
          /\ heldCmds' = r.held
          /\ drvSends' = r.sends
          /\ drvStops' = r.stops
          /\ drvTurn' = r.dturn
    /\ UNCHANGED <<stepIdx, failed, batching, holding, acts, acked,
                   disk, linkSnap, sentSnap, crashes, fails>>

\* End of Apply (moving to the next step of the pass).
Applied(ord) ==
    /\ ~failed /\ ord[stepIdx] = "apply"
    /\ stepIdx' = NextIdx(ord)
    /\ UNCHANGED <<failed, batching, holding, changed, acts, rcvd, applier, acked,
                   mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   linkSnap, sentSnap, crashes, fails>>

\* A store commit inside Apply fails (e.g. the store failed during an earlier compaction, or its
\* directory was removed): the pass stops immediately without flushing, releasing, setting state or ticking.
ApplyFail(ord, c, to) ==
    /\ ~failed /\ ord[stepIdx] = "apply"
    /\ fails < MaxFails
    /\ \/ /\ c = 0 /\ to = 0
          /\ UNCHANGED <<rcvd, applier>>
       \/ /\ c \in Clients /\ applier[c] < to /\ to <= Len(acts[c])
          /\ rcvd' = [rcvd EXCEPT ![c] = Max(rcvd[c], to)]
          /\ applier' = [applier EXCEPT ![c] = to]
    /\ failed' = TRUE
    /\ fails' = fails + 1
    /\ UNCHANGED <<stepIdx, batching, holding, changed, acts, acked,
                   mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   linkSnap, sentSnap, crashes>>

\* Flush succeeds: every record of the batch is now durable on disk.
FlushOk(ord) ==
    /\ ~failed /\ ord[stepIdx] = "flush"
    /\ stepIdx' = NextIdx(ord)
    /\ disk' = mem
    /\ batch' = <<>>
    /\ batching' = FALSE
    /\ UNCHANGED <<failed, holding, changed, acts, rcvd, applier, acked,
                   mem, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   linkSnap, sentSnap, crashes, fails>>

\* Flush fails (onFail = "stop" in the real code; witnesses test "continue" and "loop").
FlushFail(ord, onFail) ==
    /\ ~failed /\ ord[stepIdx] = "flush"
    /\ fails < MaxFails
    /\ fails' = fails + 1
    /\ batching' = FALSE
    /\ CASE onFail = "continue" ->
                /\ stepIdx' = NextIdx(ord)
                /\ failed' = FALSE
         [] onFail = "loop" ->
                /\ stepIdx' = 1
                /\ failed' = FALSE
         [] OTHER ->
                /\ stepIdx' = stepIdx
                /\ failed' = TRUE
    /\ UNCHANGED <<holding, changed, acts, rcvd, applier, acked,
                   mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   linkSnap, sentSnap, crashes>>

\* Release: Drivers::release carries out held commands in order.
Release(ord) ==
    /\ ~failed /\ ord[stepIdx] = "release"
    /\ stepIdx' = NextIdx(ord)
    /\ holding' = FALSE
    /\ LET d == DeliverNow(heldCmds, drvSends, drvStops, drvTurn) IN
       /\ heldCmds' = <<>>
       /\ drvSends' = d.sends
       /\ drvStops' = d.stops
       /\ drvTurn' = d.dturn
    /\ UNCHANGED <<failed, batching, changed, acts, rcvd, applier, acked,
                   mem, disk, batch, drvPhase, linkSnap, sentSnap, crashes, fails>>

\* State: ServerSession::setState stages Snap(mem) on every link when changed = TRUE.
Publish(ord) ==
    /\ ~failed /\ ord[stepIdx] = "state"
    /\ stepIdx' = NextIdx(ord)
    /\ linkSnap' = IF changed THEN Snap(mem) ELSE linkSnap
    /\ UNCHANGED <<failed, batching, holding, changed, acts, rcvd, applier, acked,
                   mem, disk, batch, drvPhase, drvTurn, heldCmds, drvSends, drvStops,
                   sentSnap, crashes, fails>>

----------------------------------------------------------------------------
\* Crash and recovery from the durable store at any point in any pass.
\* k in 0..Len(batch) is how many uncommitted records of the batch survived on disk:
\* 0 before Flush's write or on revert, 0..Len(batch) when a crash tears Flush's write before sync.

RecoverStore(d0) ==
    IF d0.open = 0 /\ d0.waiting = <<>> /\ d0.queue = <<>>
    THEN [st |-> d0, ph |-> "none"]
    ELSE LET dCut == IF d0.open # 0
                     THEN [d0 EXCEPT !.tr = Append(@, EntryX), !.open = 0]
                     ELSE d0
             dTak == TakeStep(dCut)
             ph   == IF dTak.waiting # <<>> THEN "starting" ELSE "none"
         IN [st |-> dTak, ph |-> ph]

Crash(k, resetApplier) ==
    /\ (failed \/ crashes < MaxCrashes)
    /\ k \in 0..Len(batch)
    /\ LET d0  == IF k = 0 THEN disk ELSE batch[k]
           rec == RecoverStore(d0)
           d1  == rec.st
       IN /\ crashes' = IF failed THEN crashes ELSE crashes + 1
          /\ stepIdx' = 1
          /\ failed' = FALSE
          /\ batching' = FALSE
          /\ holding' = FALSE
          /\ changed' = FALSE
          /\ disk' = d1
          /\ mem' = d1
          /\ batch' = <<>>
          /\ drvPhase' = rec.ph
          /\ drvTurn' = 0
          /\ heldCmds' = <<>>
          /\ rcvd' = acked
          /\ applier' = [c \in Clients |-> IF resetApplier THEN 0 ELSE d1.applied[c]]
          /\ linkSnap' = Snap(d1)
    /\ UNCHANGED <<acts, acked, drvSends, drvStops, sentSnap, fails>>

----------------------------------------------------------------------------
\* Complete transition relation, parameterized for reversed witnesses.

NextWith(ord, holdDrv, onFail, resetApplier) ==
    \/ \E c \in Clients, k \in Kinds : Act(c, k)
    \/ Tick(ord)
    \/ Hold(ord, holdDrv)
    \/ DriverHello(ord)
    \/ DriverDone(ord)
    \/ \E c \in Clients : \E to \in 1..MaxActs : RecvOnly(ord, c, to)
    \/ \E c \in Clients : \E to \in 1..MaxActs : ApplyAct(ord, c, to)
    \/ Applied(ord)
    \/ ApplyFail(ord, 0, 0)
    \/ \E c \in Clients : \E to \in 1..MaxActs : ApplyFail(ord, c, to)
    \/ FlushOk(ord)
    \/ FlushFail(ord, onFail)
    \/ Release(ord)
    \/ Publish(ord)
    \/ \E k \in 0..Len(batch) : Crash(k, resetApplier)

Next == NextWith(DefaultOrder, TRUE, "stop", FALSE)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* Safety invariants and action properties.

TypeOK ==
    /\ stepIdx \in 1..6
    /\ failed \in BOOLEAN /\ batching \in BOOLEAN /\ holding \in BOOLEAN /\ changed \in BOOLEAN
    /\ drvPhase \in {"none", "starting", "idle", "working", "stopping"}
    /\ drvTurn \in Nat
    /\ crashes \in 0..MaxCrashes
    /\ fails \in 0..MaxFails
    /\ \A c \in Clients :
        /\ Len(acts[c]) <= MaxActs
        /\ rcvd[c] \in 0..Len(acts[c])
        /\ applier[c] \in 0..Len(acts[c])
        /\ acked[c] \in 0..Len(acts[c])
        /\ disk.applied[c] \in 0..Len(acts[c])
        /\ mem.applied[c] \in 0..Len(acts[c])

\* Every client's actions are applied in order 1..applied[c], each at most once, across crashes,
\* torn batch prefixes and window reconnections.
ExactlyOnce ==
    \A c \in Clients :
        /\ disk.hist[c] = Numbers(disk.applied[c])
        /\ mem.hist[c] = Numbers(mem.applied[c])
        /\ disk.applied[c] <= mem.applied[c]

\* Each prompt is written to a driver at most once across all passes, partial flushes and crashes,
\* and turn numbers written to drivers strictly increase.
PromptAtMostOnce ==
    \A i, j \in 1..Len(drvSends) : drvSends[i].prompt = drvSends[j].prompt => i = j

TurnsIncrease ==
    \A i \in 1..Len(drvSends) :
        /\ drvSends[i].turn >= 1
        /\ i > 1 => drvSends[i].turn > drvSends[i - 1].turn

\* Nothing reaches a driver (neither a prompt's send nor a stop) before the batch that decided it is durable.
DriverOnlyDurable ==
    /\ \A i \in 1..Len(drvSends) : drvSends[i].turn < disk.next
    /\ \A i \in 1..Len(drvStops) : drvStops[i].by[2] <= disk.applied[drvStops[i].by[1]]

\* Neither the state staged on links (setState) nor any state transmitted to windows (tick) ever holds
\* uncommitted entries or uncommitted queued prompts: every entry is a durable prefix of disk.tr, and
\* every queued prompt is durable on disk.
SnapDurable(sn, d) ==
    /\ IsPrefix(sn.tr, d.tr)
    /\ \A i \in 1..Len(sn.queue) : sn.queue[i][2] <= d.applied[sn.queue[i][1]]

LinkStateDurable ==
    /\ SnapDurable(linkSnap, disk)
    /\ SnapDurable(sentSnap, disk)

\* No client action is ever acknowledged by a link tick before its batch is durable on disk, so no
\* acknowledged action is ever lost across a crash.
AckedOnlyDurable ==
    \A c \in Clients :
        /\ acked[c] <= disk.applied[c]
        /\ acked[c] <= mem.applied[c]

\* Durable transcripts and durable applied counters only grow, even across crashes and recoveries;
\* acknowledgements never go backward.
DurableMonotonic ==
    [][/\ IsPrefix(disk.tr, disk'.tr)
       /\ disk'.next >= disk.next
       /\ \A c \in Clients :
            /\ disk'.applied[c] >= disk.applied[c]
            /\ acked'[c] >= acked[c]]_vars

\* Release, State (with changes) and Tick never advance while an unflushed batch has uncommitted records.
NoOutBeforeFlush ==
    [][/\ (drvSends' # drvSends \/ drvStops' # drvStops) => batch' = <<>>
       /\ linkSnap' # linkSnap => batch' = <<>>
       /\ acked' # acked => batch' = <<>>]_vars

=============================================================================
