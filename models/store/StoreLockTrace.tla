--------------------------- MODULE StoreLockTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real start           *)
(* (src/server/start_order.h: StartOrder, through take_step, the dispatch  *)
(* Server::run takes its steps by) and the real store's lock on the real   *)
(* filesystem (PosixFs::lock: flock on the store's directory, through      *)
(* Store::open; a reader's shared lock) is a behaviour of StoreLock.tla.   *)
(*                                                                         *)
(* tests/store/store_lock_walk.cpp writes StoreLockTraceData.tla beside    *)
(* this module. Its Trace has one record per step of the walk: op, the     *)
(* process p, and after the step each process's state (pcs: p1, p2, r1),   *)
(* how many hold the store's lock exclusive (holders) and shared (readers),*)
(* asked of the system, whose the socket's file is (sock), the record      *)
(* numbers in the log as they lie on the disk (seqs), and the sends of the *)
(* queued prompt.                                                          *)
(*                                                                         *)
(* Each step takes the StoreLock action its op names, for its process, and *)
(* the state it reaches must be the code's: a step the spec cannot take,   *)
(* or takes to another state, has no successor, and TLC reports a          *)
(* deadlock (an open with no lock before it, a restore before the socket,  *)
(* a held lock refused at once, readers taken for a writer). Every         *)
(* StoreLock invariant is checked on the trace too.                        *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC, StoreLockTraceData

VARIABLES pc, at, cur, lock, shared, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent,
          starts, reads, stops, deaths, l

Unbounded == 1000000
Names == <<"p1", "p2", "r1">>

\* The walk's waits: three retries on readers, three tries of a held lock (tests/store/store_lock_walk.cpp).
S == INSTANCE StoreLock WITH Writers <- {"p1", "p2"}, Readers <- {"r1"}, MaxStarts <- Unbounded,
                             MaxReads <- Unbounded, MaxWrites <- Unbounded, MaxStops <- Unbounded,
                             MaxDeaths <- Unbounded, MaxMoves <- 0, ReaderWait <- 3, HeldWait <- 3, Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ [i \in 1..Len(Names) |-> pc[Names[i]]] = r.pcs
    /\ Cardinality(lock[0]) = r.holders
    /\ Cardinality(shared[0]) = r.readers
    /\ sockFile[0] = r.sock
    /\ [i \in 1..Len(journal[0]) |-> journal[0][i].seq] = r.seqs
    /\ sent[0] = r.sent

TraceMatches == Matches(l)

Step(r) ==
    IF r.op = "start" THEN S!Start(r.p)
    ELSE IF r.op = "restart" THEN S!Restart(r.p)
    ELSE IF r.op = "probe" THEN S!Probe(r.p)
    ELSE IF r.op \in {"rwait", "lock"} THEN S!LockTry(r.p)
    ELSE IF r.op = "heldtick" THEN S!HeldTick(r.p)
    ELSE IF r.op = "open" THEN S!Open(r.p)
    ELSE IF r.op = "listen" THEN S!Listen(r.p)
    ELSE IF r.op = "restore" THEN S!Restore(r.p)
    ELSE IF r.op = "write" THEN S!Write(r.p)
    ELSE IF r.op = "stop" THEN S!Stop(r.p)
    ELSE IF r.op = "end" THEN S!EndDrivers(r.p)
    ELSE IF r.op = "exit" THEN S!Exit(r.p)
    ELSE IF r.op = "die" THEN S!Die(r.p)
    ELSE IF r.op = "rlock" THEN S!RLock(r.p)
    ELSE IF r.op = "rrelease" THEN S!RRelease(r.p)
    ELSE FALSE

traceVars == <<pc, at, cur, lock, shared, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent,
               starts, reads, stops, deaths, l>>

TraceInit == l = 1 /\ S!Init /\ TraceMatches

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])
    /\ TraceMatches'

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
SocketKept == S!SocketKept

=============================================================================
