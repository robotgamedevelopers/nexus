----------------------------- MODULE StoreLock -----------------------------
(***************************************************************************)
(* The store's lock and nexus-server's start and stop                      *)
(* (src/server/start_order.h, src/server/server.cpp Server::run,           *)
(* src/store/store.cpp Store::open and Store::inspect, src/store/fs.h      *)
(* Fs::lock): several nexus-server processes (Writers) and readers         *)
(* (Readers: `nexus-server store check`), each of which may start, take    *)
(* its steps, and die at any point, on one store.                          *)
(*                                                                         *)
(* A writer's start, in the code's order:                                  *)
(*   Probe    a server answers on the control socket: refused (exit 0)     *)
(*   LockTry  the lock, exclusive, on the store's directory: taken when    *)
(*            free; a writer holds it: Held, and the start waits (HeldTick)*)
(*            ; only readers hold it: tried again, ReaderWait times, then  *)
(*            the start fails (exit 1)                                     *)
(*   HeldTick a wait's try: a server answers, refused; else LockTry again, *)
(*            HeldWait tries in all, then Stuck (exit 3)                   *)
(*   Open     the replay: the process's view of the journal is what is     *)
(*            there now, and whether the queued prompt was sent            *)
(*   Listen   the control socket: refused when a server answers on it;     *)
(*            else its file (a stale one removed) is this process's        *)
(*   Restore  the drivers restored: one record, which marks the queued     *)
(*            prompt sent when this process's view says it is still        *)
(*            queued (a driver started for it, the prompt written once)    *)
(* then Write, a record per step of the running server; and its stop:      *)
(*   Stop       the socket closed and its file removed, if the name still  *)
(*              names this process's socket                                *)
(*   EndDrivers the drivers ended: their last record, under the lock       *)
(*   Exit       the process ends: its lock released by the system          *)
(* A refused, failed or stuck start exits too. An exit 0 (refused,         *)
(* stopped) is not started again; any other end (failed, stuck, Die: kill  *)
(* -9, a crash) is, by the service (Restart). Die leaves its socket's file *)
(* behind, answering no one.                                               *)
(*                                                                         *)
(* A reader takes the lock shared, when no writer holds it, and lets go    *)
(* (RRelease: its copy ends, or outlasts its limit and is given up).       *)
(*                                                                         *)
(* The store is a directory that may be moved aside, with a new one made   *)
(* at its name (MoveAside): Gens numbers them, cur the one at the name     *)
(* now. A process holds the one it locked (at), by its descriptor; the     *)
(* socket is in the same server directory, so it moves with it. Each store *)
(* has its lock, its readers, its socket file and its journal: a sequence  *)
(* of records, each with the number its writer gave it (one past its       *)
(* view), and whether its writer held that store's lock and had bound its  *)
(* socket in this run when it wrote it.                                    *)
(*                                                                         *)
(* What the journal does with its records (the log, the snapshot, torn     *)
(* tails, crashes of the machine) is Journal.tla; here a record is only    *)
(* its number and its writer. A server from before the lock (no lock at    *)
(* all) beside one of this build is outside the model: the code's Probe is *)
(* there for it, and the model keeps the step (docs/formats.md).           *)
(*                                                                         *)
(* Bug selects a broken build for the reversed witnesses:                  *)
(*   "unlocked"        the order before the lock: open, restore, listen    *)
(*   "restore_first"   the lock kept, the drivers restored before the      *)
(*                     socket                                              *)
(*   "held_refused"    a lock held by a writer taken as another server:    *)
(*                     exit 0 at once (a server stopping leaves nothing)   *)
(*   "reader_refused"  readers that do not let go taken as another server: *)
(*                     exit 0 (nothing left serving, nothing restarted)    *)
(*   "by_path"         the lock taken on the store, its writes made by its *)
(*                     name: once moved, into the store now at the name    *)
(*   "unlink_by_path"  the stop removes whatever socket file the name      *)
(*                     names: once moved, another server's                 *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Writers, Readers, MaxStarts, MaxReads, MaxWrites, MaxStops, MaxDeaths, MaxMoves, ReaderWait, HeldWait, Bug

ASSUME Bug \in {"none", "unlocked", "restore_first", "held_refused", "reader_refused", "by_path", "unlink_by_path"}
ASSUME ReaderWait \in Nat /\ HeldWait \in Nat \ {0} /\ MaxMoves \in Nat

None == "none"
Gens == 0..MaxMoves

VARIABLES pc, at, cur, lock, shared, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent,
          starts, reads, stops, deaths

vars == <<pc, at, cur, lock, shared, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent,
          starts, reads, stops, deaths>>

WriterStates == {"down", "crashed", "start", "probed", "readers", "held", "locked", "open", "listening", "restored",
                 "running", "closed", "ended", "refused", "failed", "stuck"}
ReaderStates == {"rdown", "rheld"}
\* A writer that has the store open, for reading what it replayed and for writing.
Opened == {"open", "listening", "restored", "running", "closed", "ended"}
\* A writer whose socket answers.
Bound == {"listening", "running"}

\* The record each store held before any of these processes: one session with a prompt queued.
First == [seq |-> 1, kind |-> "init", locked |-> TRUE, listened |-> TRUE]

SentIn(j) == \E i \in 1..Len(j) : j[i].kind = "sent"

TypeOK ==
    /\ pc \in [Writers \cup Readers -> WriterStates \cup ReaderStates]
    /\ at \in [Writers \cup Readers -> Gens]
    /\ cur \in Gens
    /\ lock \in [Gens -> SUBSET Writers]
    /\ shared \in [Gens -> SUBSET Readers]
    /\ sockFile \in [Gens -> Writers \cup {None}]
    /\ sockAt \in [Writers -> Gens]
    /\ view \in [Writers -> Nat]
    /\ seen \in [Writers -> BOOLEAN]
    /\ bound \in [Writers -> BOOLEAN]
    /\ rwait \in [Writers -> Nat]
    /\ hwait \in [Writers -> Nat]
    /\ sent \in [Gens -> Nat]
    /\ starts \in [Writers -> 0..MaxStarts]
    /\ reads \in [Readers -> 0..MaxReads]
    /\ stops \in 0..MaxStops
    /\ deaths \in 0..MaxDeaths

Init ==
    /\ pc = [p \in Writers \cup Readers |-> IF p \in Writers THEN "down" ELSE "rdown"]
    /\ at = [p \in Writers \cup Readers |-> 0]
    /\ cur = 0
    /\ lock = [g \in Gens |-> {}]
    /\ shared = [g \in Gens |-> {}]
    /\ sockFile = [g \in Gens |-> None]
    /\ sockAt = [p \in Writers |-> 0]
    /\ journal = [g \in Gens |-> <<First>>]
    /\ view = [p \in Writers |-> 0]
    /\ seen = [p \in Writers |-> FALSE]
    /\ bound = [p \in Writers |-> FALSE]
    /\ rwait = [p \in Writers |-> 0]
    /\ hwait = [p \in Writers |-> 0]
    /\ sent = [g \in Gens |-> 0]
    /\ starts = [p \in Writers |-> 0]
    /\ reads = [r \in Readers |-> 0]
    /\ stops = 0
    /\ deaths = 0

\* A server answers on the socket at the store's name now: its file there is a live server's.
Answers == sockFile[cur] # None /\ pc[sockFile[cur]] \in Bound

\* Where each step starts and ends, by the order Bug selects.
OpenFrom == IF Bug = "unlocked" THEN "start" ELSE "locked"
ListenFrom == IF Bug \in {"unlocked", "restore_first"} THEN "restored" ELSE "open"
ListenTo == IF Bug \in {"unlocked", "restore_first"} THEN "running" ELSE "listening"
RestoreFrom == IF Bug \in {"unlocked", "restore_first"} THEN "open" ELSE "listening"
RestoreTo == IF Bug \in {"unlocked", "restore_first"} THEN "restored" ELSE "running"

\* The store p's records go to: the one it holds, through its descriptor; by its name (Bug "by_path"), the one there now.
WG(p) == IF Bug = "by_path" THEN cur ELSE at[p]

\* One record written by p, numbered one past what p replayed and wrote.
Record(p, kind) == [seq |-> view[p] + 1, kind |-> kind, locked |-> p \in lock[WG(p)], listened |-> bound[p]]

Start(p) ==
    /\ pc[p] = "down"
    /\ starts[p] < MaxStarts
    /\ pc' = [pc EXCEPT ![p] = "start"]
    /\ starts' = [starts EXCEPT ![p] = @ + 1]
    /\ hwait' = [hwait EXCEPT ![p] = 0]
    /\ bound' = [bound EXCEPT ![p] = FALSE]
    /\ UNCHANGED <<at, cur, lock, shared, sockFile, sockAt, journal, view, seen, rwait, sent, reads, stops, deaths>>

\* The service starts again a process that ended other than with exit 0.
Restart(p) ==
    /\ pc[p] = "crashed"
    /\ pc' = [pc EXCEPT ![p] = "start"]
    /\ hwait' = [hwait EXCEPT ![p] = 0]
    /\ bound' = [bound EXCEPT ![p] = FALSE]
    /\ UNCHANGED <<at, cur, lock, shared, sockFile, sockAt, journal, view, seen, rwait, sent, starts, reads, stops,
                   deaths>>

Probe(p) ==
    /\ Bug # "unlocked"
    /\ pc[p] = "start"
    /\ pc' = [pc EXCEPT ![p] = IF Answers THEN "refused" ELSE "probed"]
    /\ rwait' = [rwait EXCEPT ![p] = 0]
    /\ UNCHANGED <<at, cur, lock, shared, sockFile, sockAt, journal, view, seen, bound, hwait, sent, starts, reads,
                   stops, deaths>>

\* One try at the lock on the store at the name now (Fs::lock's loop: each refusal by readers alone is a pause).
LockTry(p) ==
    /\ Bug # "unlocked"
    /\ pc[p] \in {"probed", "readers"}
    /\ IF lock[cur] = {} /\ shared[cur] = {}
          THEN /\ lock' = [lock EXCEPT ![cur] = @ \cup {p}]
               /\ at' = [at EXCEPT ![p] = cur]
               /\ pc' = [pc EXCEPT ![p] = "locked"]
               /\ UNCHANGED rwait
          ELSE IF lock[cur] # {}
          THEN /\ pc' = [pc EXCEPT ![p] = IF Bug = "held_refused" THEN "refused" ELSE "held"]
               /\ UNCHANGED <<lock, at, rwait>>
          ELSE IF rwait[p] < ReaderWait
          THEN /\ pc' = [pc EXCEPT ![p] = "readers"]
               /\ rwait' = [rwait EXCEPT ![p] = @ + 1]
               /\ UNCHANGED <<lock, at>>
          ELSE /\ pc' = [pc EXCEPT ![p] = IF Bug = "reader_refused" THEN "refused" ELSE "failed"]
               /\ UNCHANGED <<lock, at, rwait>>
    /\ UNCHANGED <<cur, shared, sockFile, sockAt, journal, view, seen, bound, hwait, sent, starts, reads, stops, deaths>>

\* A try of the wait for a lock a writer holds: a pause, then the socket asked.
HeldTick(p) ==
    /\ pc[p] = "held"
    /\ IF Answers
          THEN /\ pc' = [pc EXCEPT ![p] = "refused"]
               /\ UNCHANGED <<hwait, rwait>>
          ELSE IF hwait[p] + 1 >= HeldWait
          THEN /\ pc' = [pc EXCEPT ![p] = "stuck"]
               /\ UNCHANGED <<hwait, rwait>>
          ELSE /\ pc' = [pc EXCEPT ![p] = "probed"]
               /\ hwait' = [hwait EXCEPT ![p] = @ + 1]
               /\ rwait' = [rwait EXCEPT ![p] = 0]
    /\ UNCHANGED <<at, cur, lock, shared, sockFile, sockAt, journal, view, seen, bound, sent, starts, reads, stops,
                   deaths>>

Open(p) ==
    /\ pc[p] = OpenFrom
    /\ LET g == IF Bug = "unlocked" THEN cur ELSE at[p] IN
       /\ at' = [at EXCEPT ![p] = g]
       /\ view' = [view EXCEPT ![p] = Len(journal[g])]
       /\ seen' = [seen EXCEPT ![p] = SentIn(journal[g])]
    /\ pc' = [pc EXCEPT ![p] = "open"]
    /\ UNCHANGED <<cur, lock, shared, sockFile, sockAt, journal, bound, rwait, hwait, sent, starts, reads, stops,
                   deaths>>

Listen(p) ==
    /\ pc[p] = ListenFrom
    /\ IF Answers
          THEN /\ pc' = [pc EXCEPT ![p] = "refused"]
               /\ UNCHANGED <<sockFile, sockAt, bound>>
          ELSE /\ sockFile' = [sockFile EXCEPT ![cur] = p]
               /\ sockAt' = [sockAt EXCEPT ![p] = cur]
               /\ bound' = [bound EXCEPT ![p] = TRUE]
               /\ pc' = [pc EXCEPT ![p] = ListenTo]
    /\ UNCHANGED <<at, cur, lock, shared, journal, view, seen, rwait, hwait, sent, starts, reads, stops, deaths>>

Restore(p) ==
    /\ pc[p] = RestoreFrom
    /\ journal' = [journal EXCEPT ![WG(p)] = Append(@, Record(p, IF seen[p] THEN "driver" ELSE "sent"))]
    /\ sent' = IF seen[p] THEN sent ELSE [sent EXCEPT ![WG(p)] = @ + 1]
    /\ seen' = [seen EXCEPT ![p] = TRUE]
    /\ view' = [view EXCEPT ![p] = @ + 1]
    /\ pc' = [pc EXCEPT ![p] = RestoreTo]
    /\ UNCHANGED <<at, cur, lock, shared, sockFile, sockAt, bound, rwait, hwait, starts, reads, stops, deaths>>

Write(p) ==
    /\ pc[p] = "running"
    /\ Len(journal[WG(p)]) < MaxWrites
    /\ journal' = [journal EXCEPT ![WG(p)] = Append(@, Record(p, "step"))]
    /\ view' = [view EXCEPT ![p] = @ + 1]
    /\ UNCHANGED <<pc, at, cur, lock, shared, sockFile, sockAt, seen, bound, rwait, hwait, sent, starts, reads, stops,
                   deaths>>

\* `nexus-server stop`: the socket closed, and its file removed if the name still names this server's.
Stop(p) ==
    /\ pc[p] = "running"
    /\ stops < MaxStops
    /\ sockFile' = IF Bug = "unlink_by_path" \/ sockFile[cur] = p THEN [sockFile EXCEPT ![cur] = None] ELSE sockFile
    /\ pc' = [pc EXCEPT ![p] = "closed"]
    /\ stops' = stops + 1
    /\ UNCHANGED <<at, cur, lock, shared, sockAt, journal, view, seen, bound, rwait, hwait, sent, starts, reads,
                   deaths>>

\* The drivers ended, after the socket closed: their last record, under the lock, still held.
EndDrivers(p) ==
    /\ pc[p] = "closed"
    /\ journal' = [journal EXCEPT ![WG(p)] = Append(@, Record(p, "end"))]
    /\ view' = [view EXCEPT ![p] = @ + 1]
    /\ pc' = [pc EXCEPT ![p] = "ended"]
    /\ UNCHANGED <<at, cur, lock, shared, sockFile, sockAt, seen, bound, rwait, hwait, sent, starts, reads, stops,
                   deaths>>

ReleaseAll(p) == [g \in Gens |-> lock[g] \ {p}]

\* The process ends, with the exit its state gives: 0 (refused, stopped) is not started again; 1 or 3 is, by the
\* service. A failed start removes its own socket's file, if it had one.
Exit(p) ==
    /\ pc[p] \in {"refused", "ended", "failed", "stuck"}
    /\ lock' = ReleaseAll(p)
    /\ pc' = [pc EXCEPT ![p] = IF pc[p] \in {"failed", "stuck"} THEN "crashed" ELSE "down"]
    /\ sockFile' = IF pc[p] = "failed" /\ bound[p] /\ sockFile[cur] = p THEN [sockFile EXCEPT ![cur] = None]
                                                                     ELSE sockFile
    /\ bound' = [bound EXCEPT ![p] = FALSE]
    /\ UNCHANGED <<at, cur, shared, sockAt, journal, view, seen, rwait, hwait, sent, starts, reads, stops, deaths>>

\* kill -9, or a crash, at any time: the system releases its lock; its socket's file stays, answering no one.
Die(p) ==
    /\ pc[p] \notin {"down", "crashed"}
    /\ deaths < MaxDeaths
    /\ pc' = [pc EXCEPT ![p] = "crashed"]
    /\ lock' = ReleaseAll(p)
    /\ bound' = [bound EXCEPT ![p] = FALSE]
    /\ deaths' = deaths + 1
    /\ UNCHANGED <<at, cur, shared, sockFile, sockAt, journal, view, seen, rwait, hwait, sent, starts, reads, stops>>

\* A reader: the lock shared when no writer holds it (else refused, having read nothing), then let go.
RLock(r) ==
    /\ pc[r] = "rdown"
    /\ reads[r] < MaxReads
    /\ reads' = [reads EXCEPT ![r] = @ + 1]
    /\ IF lock[cur] = {}
          THEN /\ shared' = [shared EXCEPT ![cur] = @ \cup {r}]
               /\ at' = [at EXCEPT ![r] = cur]
               /\ pc' = [pc EXCEPT ![r] = "rheld"]
          ELSE UNCHANGED <<shared, at, pc>>
    /\ UNCHANGED <<cur, lock, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent, starts, stops, deaths>>

RRelease(r) ==
    /\ pc[r] = "rheld"
    /\ shared' = [shared EXCEPT ![at[r]] = @ \ {r}]
    /\ pc' = [pc EXCEPT ![r] = "rdown"]
    /\ UNCHANGED <<at, cur, lock, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent, starts, reads,
                   stops, deaths>>

\* The server's directory moved aside, and a new one (a store as the first was) made at its name.
MoveAside ==
    /\ cur < MaxMoves
    /\ cur' = cur + 1
    /\ UNCHANGED <<pc, at, lock, shared, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent, starts,
                   reads, stops, deaths>>

\* A writer's own steps, which it takes unless it dies: under weak fairness in Live.
Progress(p) == Restart(p) \/ Probe(p) \/ LockTry(p) \/ HeldTick(p) \/ Open(p) \/ Listen(p) \/ Restore(p) \/
               EndDrivers(p) \/ Exit(p)

\* Nothing left to do: every writer down with its starts spent, or serving with its writes, the stops and the deaths
\* spent; every reader done. The end of a behaviour, not a deadlock.
Spent(p) == \/ pc[p] = "down" /\ starts[p] = MaxStarts
            \/ pc[p] = "running" /\ Len(journal[WG(p)]) >= MaxWrites /\ stops = MaxStops /\ deaths = MaxDeaths
Finished == /\ \A p \in Writers : Spent(p)
            /\ \A r \in Readers : pc[r] = "rdown" /\ reads[r] = MaxReads
            /\ UNCHANGED vars

Next == \/ \E p \in Writers : Start(p) \/ Progress(p) \/ Write(p) \/ Stop(p) \/ Die(p)
        \/ \E r \in Readers : RLock(r) \/ RRelease(r)
        \/ MoveAside
        \/ Finished

Spec == Init /\ [][Next]_vars

\* Every writer takes its own steps, and every reader lets go (its copy's limit), however others interleave; a death,
\* a stop, a start by a person, a write, a reader's lock and a move happen or not.
Live == Spec /\ (\A p \in Writers : WF_vars(Progress(p))) /\ (\A r \in Readers : WF_vars(RRelease(r)))

----------------------------------------------------------------------------
(* Properties *)

\* At most one process has each store open.
OneWriter == \A g \in Gens : Cardinality({p \in Writers : pc[p] \in Opened /\ at[p] = g}) <= 1

\* A process with a store open holds that store's lock, alone.
OpenHoldsLock == \A p \in Writers : pc[p] \in Opened => lock[at[p]] = {p}

\* No record is written by a process that does not hold that store's lock.
LockedWrites == \A g \in Gens : \A i \in 1..Len(journal[g]) : journal[g][i].locked

\* Nothing is written before the socket was bound, in the writer's run (the drivers are restored only then); the
\* drivers' last records at a stop come after the socket closed, and the lock is held still (LockedWrites).
ListenedWrites == \A g \in Gens : \A i \in 1..Len(journal[g]) : journal[g][i].listened

\* The records are numbered 1, 2, 3, ... with no number twice: what the journal's next open needs (Journal.tla).
SeqsUnique == \A g \in Gens : \A i \in 1..Len(journal[g]) : journal[g][i].seq = i

\* Each store's queued prompt is written to a driver at most once.
SentOnce == \A g \in Gens : sent[g] <= 1

\* Each lock has at most one writer, and never a writer and readers at once.
Holders == \A g \in Gens : Cardinality(lock[g]) <= 1 /\ (lock[g] = {} \/ shared[g] = {})

\* A server whose socket answers still has its socket's file: none was removed by another process.
SocketKept == \A q \in Writers : pc[q] \in Bound => sockFile[sockAt[q]] = q

\* A start is never left with nothing serving: once a writer is starting, some writer serves then or later. What
\* would have caught a start that exits 0 when a stopping server holds the lock, or when readers do not let go.
Serves == (\E p \in Writers : pc[p] = "start") ~> (\E q \in Writers : pc[q] = "running")

=============================================================================
