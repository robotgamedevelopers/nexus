--------------------------- MODULE StoreLockTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real start-up order  *)
(* (src/server/start_order.h) and the real store's lock (Store::open on    *)
(* MemFs) is a behaviour of StoreLock.tla.                                 *)
(*                                                                         *)
(* tests/store/store_lock_walk.cpp writes StoreLockTraceData.tla beside    *)
(* this module. Its Trace has one record per step of the walk: op (init,   *)
(* start, probe, lock, open, listen, restore, write, die), the process p,  *)
(* for a lock whether it was had (ok), and after the step each process's   *)
(* state (pcs, from its StartOrder), how many hold the store's lock        *)
(* (holders, from MemFs), who holds the socket, the record numbers in the  *)
(* log as they lie on the disk (seqs), and the sends of the queued prompt. *)
(*                                                                         *)
(* Each step takes the StoreLock action its op names, for its process;    *)
(* TraceMatches requires the states, the lock, the socket, the log's       *)
(* numbers and the sends to be the code's. A step the spec cannot take has *)
(* no successor: TLC reports a deadlock (an open with no lock before it,  *)
(* a restore before the socket). Every StoreLock invariant is checked on   *)
(* the trace too.                                                          *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC, StoreLockTraceData

VARIABLES pc, lock, sock, journal, view, seen, sent, starts, l

Unbounded == 1000000
ProcList == <<"p1", "p2">>

S == INSTANCE StoreLock WITH Procs <- {"p1", "p2"}, MaxStarts <- Unbounded, MaxWrites <- Unbounded, Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ r.op # "lock" => [i \in 1..Len(ProcList) |-> pc[ProcList[i]]] = r.pcs
    /\ Cardinality(lock) = r.holders
    /\ r.op = "lock" => (r.ok <=> r.p \in lock)
    /\ sock = r.sock
    /\ [i \in 1..Len(journal) |-> journal[i].seq] = r.seqs
    /\ sent = r.sent

TraceMatches == Matches(l)

Step(r) ==
    IF r.op = "start" THEN S!Start(r.p)
    ELSE IF r.op = "probe" THEN S!Probe(r.p)
    ELSE IF r.op = "lock" THEN S!Lock(r.p)
    ELSE IF r.op = "open" THEN S!Open(r.p)
    ELSE IF r.op = "listen" THEN S!Listen(r.p)
    ELSE IF r.op = "restore" THEN S!Restore(r.p)
    ELSE IF r.op = "write" THEN S!Write(r.p)
    ELSE IF r.op = "die" THEN S!Die(r.p)
    ELSE FALSE

traceVars == <<pc, lock, sock, journal, view, seen, sent, starts, l>>

TraceInit == l = 1 /\ S!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == S!TypeOK
OneWriter == S!OneWriter
OpenHoldsLock == S!OpenHoldsLock
LockedWrites == S!LockedWrites
ListenedWrites == S!ListenedWrites
SeqsUnique == S!SeqsUnique
SentOnce == S!SentOnce
Holders == S!Holders

=============================================================================
