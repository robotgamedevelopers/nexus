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
(*   Probe    a server says it is serving on the control socket (it is     *)
(*            asked `status` and answers SERVING from its serving loop):   *)
(*            refused (exit 0). A connection alone is not an answer: a     *)
(*            server that has accepted `stop` still holds its socket       *)
(*   LockTry  the gate, then the lock, exclusive, on the store's           *)
(*            directory: taken when free; a writer holds it, or another    *)
(*            writer holds the gate: Held, and the start waits (HeldTick); *)
(*            only readers hold it: the gate kept (no new reader starts),  *)
(*            tried again, ReaderWait times, then the start fails (exit 1) *)
(*   HeldTick a wait's try: a server answers, refused; else LockTry again, *)
(*            HeldWait tries in all, then Stuck (exit 3)                   *)
(*   Open     the replay: the process's view of the journal is what is     *)
(*            there now, and whether the queued prompt was sent            *)
(*   Listen   the control socket: refused when a server answers on it;     *)
(*            else its file (a stale one removed) is this process's        *)
(*   Restore  the drivers restored: one record, which marks the queued     *)
(*            prompt sent when this process's view says it is still        *)
(*            queued (a driver started for it, the prompt written once)    *)
(* Open, Listen and Restore may each fail instead (OpenFail, ListenFail,   *)
(* RestoreFail: a store that does not read, a socket that cannot be bound, *)
(* a commit refused): exit 1, the lock released, its own socket removed.   *)
(* Then Write, a record per step of the running server; and its stop:      *)
(*   StopAccept `nexus-server stop` (or a signal) taken: from now on the   *)
(*              server answers no request, though its socket is still      *)
(*              bound                                                      *)
(*   CloseSocket the socket closed and its file removed, if the file in    *)
(*              its own directory is still this process's socket           *)
(*   EndDrivers the drivers ended: their last record, under the lock       *)
(*   Exit       the process ends: its lock released by the system          *)
(* A refused, failed or stuck start exits too. An exit 0 (refused,         *)
(* stopped) is not started again; any other end (failed, stuck, Die: kill  *)
(* -9, a crash) is, by the service (Restart). Die leaves its socket's file *)
(* behind, answering no one.                                               *)
(*                                                                         *)
(* A reader takes the lock shared, when no writer holds it and no writer   *)
(* holds the gate (it gives way to a writer waiting), and lets go          *)
(* (RRelease: its copy ends, or outlasts its limit and is given up).       *)
(* MaxReads 0 is no bound on reads: a stream of readers, each overlapping  *)
(* the next.                                                               *)
(*                                                                         *)
(* The store is a directory that may be moved aside, with a new one made   *)
(* at its name (MoveAside): Gens numbers them, cur the one at the name     *)
(* now. A writer holds the one at the name when it started (at), by its    *)
(* descriptor: its question to the socket, its gate, its lock, its store   *)
(* and its socket are all in that one; a reader takes the one at the name. *)
(* Each store has its lock, its gate, its readers, its socket file and its *)
(* journal: a sequence                                                     *)
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
(*   "connect_probe"   a connection taken as a server serving: one that is *)
(*                     restoring or has accepted `stop` "answers", and a   *)
(*                     start exits 0 with nothing left serving             *)
(*   "no_gate"         a reader takes the lock while a writer waits for    *)
(*                     readers, so a stream of readers starves the writer  *)
(*                                                                         *)
(* excused is a ghost (no code holds it): a refused start whose server     *)
(* was then stopped on purpose is not owed a server (RefusedServes).       *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Writers, Readers, MaxStarts, MaxReads, MaxWrites, MaxStops, MaxDeaths, MaxMoves, MaxFails, ReaderWait,
          HeldWait, Bug

ASSUME Bug \in {"none", "unlocked", "restore_first", "held_refused", "reader_refused", "by_path", "unlink_by_path",
                "connect_probe", "no_gate"}
ASSUME ReaderWait \in Nat /\ HeldWait \in Nat \ {0} /\ MaxMoves \in Nat /\ MaxFails \in Nat /\ MaxReads \in Nat

None == "none"
Gens == 0..MaxMoves

VARIABLES pc, at, cur, lock, shared, gate, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent,
          starts, reads, stops, deaths, fails, excused

vars == <<pc, at, cur, lock, shared, gate, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent,
          starts, reads, stops, deaths, fails, excused>>

WriterStates == {"down", "crashed", "start", "probed", "readers", "held", "locked", "open", "listening", "restored",
                 "running", "stopping", "closed", "ended", "refused", "failed", "stuck"}
ReaderStates == {"rdown", "rheld"}
\* A writer that has the store open, for reading what it replayed and for writing.
Opened == {"open", "listening", "restored", "running", "stopping", "closed", "ended"}
\* A writer whose socket is bound (its file is its own while it is: SocketKept).
Bound == {"listening", "running", "stopping"}
\* A writer that says it is serving when asked: only from its serving loop, before it accepts `stop`. A connection
\* alone reaches any bound socket (Bug "connect_probe").
Serving == IF Bug = "connect_probe" THEN Bound ELSE {"running"}

\* The record each store held before any of these processes: one session with a prompt queued.
First == [seq |-> 1, kind |-> "init", locked |-> TRUE, listened |-> TRUE]

SentIn(j) == \E i \in 1..Len(j) : j[i].kind = "sent"

TypeOK ==
    /\ pc \in [Writers \cup Readers -> WriterStates \cup ReaderStates]
    /\ at \in [Writers \cup Readers -> Gens]
    /\ cur \in Gens
    /\ lock \in [Gens -> SUBSET Writers]
    /\ shared \in [Gens -> SUBSET Readers]
    /\ gate \in [Gens -> SUBSET Writers]
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
    /\ fails \in 0..MaxFails
    /\ excused \in [Writers -> BOOLEAN]

Init ==
    /\ pc = [p \in Writers \cup Readers |-> IF p \in Writers THEN "down" ELSE "rdown"]
    /\ at = [p \in Writers \cup Readers |-> 0]
    /\ cur = 0
    /\ lock = [g \in Gens |-> {}]
    /\ shared = [g \in Gens |-> {}]
    /\ gate = [g \in Gens |-> {}]
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
    /\ fails = 0
    /\ excused = [p \in Writers |-> FALSE]

\* A server says it is serving on the socket in p's server directory (held by p since its start, at[p]): its file
\* there is a server's in Serving.
Answers(p) == sockFile[at[p]] # None /\ pc[sockFile[at[p]]] \in Serving

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

\* p's refusal is its own, not excused, from the moment it is refused.
Unexcused(p) == excused' = [excused EXCEPT ![p] = FALSE]

Start(p) ==
    /\ pc[p] = "down"
    /\ starts[p] < MaxStarts
    /\ pc' = [pc EXCEPT ![p] = "start"]
    /\ starts' = [starts EXCEPT ![p] = @ + 1]
    /\ hwait' = [hwait EXCEPT ![p] = 0]
    /\ bound' = [bound EXCEPT ![p] = FALSE]
    /\ at' = [at EXCEPT ![p] = cur]
    /\ UNCHANGED <<cur, lock, shared, gate, sockFile, sockAt, journal, view, seen, rwait, sent, reads, stops,
                   deaths, fails, excused>>

\* The service starts again a process that ended other than with exit 0.
Restart(p) ==
    /\ pc[p] = "crashed"
    /\ pc' = [pc EXCEPT ![p] = "start"]
    /\ hwait' = [hwait EXCEPT ![p] = 0]
    /\ bound' = [bound EXCEPT ![p] = FALSE]
    /\ at' = [at EXCEPT ![p] = cur]
    /\ UNCHANGED <<cur, lock, shared, gate, sockFile, sockAt, journal, view, seen, rwait, sent, starts, reads,
                   stops, deaths, fails, excused>>

Probe(p) ==
    /\ Bug # "unlocked"
    /\ pc[p] = "start"
    /\ pc' = [pc EXCEPT ![p] = IF Answers(p) THEN "refused" ELSE "probed"]
    /\ rwait' = [rwait EXCEPT ![p] = 0]
    /\ Unexcused(p)
    /\ UNCHANGED <<at, cur, lock, shared, gate, sockFile, sockAt, journal, view, seen, bound, hwait, sent, starts,
                   reads, stops, deaths, fails>>

\* p no longer holds any gate.
GateGone(p) == [g \in Gens |-> gate[g] \ {p}]

\* One try at the lock (Fs::lock's loop: each refusal by readers alone is a pause, with the gate kept). The gate first:
\* another writer holding it is a writer opening the store, so Held, as a lock a writer holds.
LockTry(p) ==
    /\ Bug # "unlocked"
    /\ pc[p] \in {"probed", "readers"}
    /\ LET g == at[p] IN
       IF Bug # "no_gate" /\ gate[g] \ {p} # {}
          THEN /\ pc' = [pc EXCEPT ![p] = IF Bug = "held_refused" THEN "refused" ELSE "held"]
               /\ gate' = GateGone(p)
               /\ UNCHANGED <<lock, rwait>>
       ELSE IF lock[g] = {} /\ shared[g] = {}
          THEN /\ lock' = [lock EXCEPT ![g] = @ \cup {p}]
               /\ pc' = [pc EXCEPT ![p] = "locked"]
               /\ gate' = GateGone(p)
               /\ UNCHANGED rwait
       ELSE IF lock[g] # {}
          THEN /\ pc' = [pc EXCEPT ![p] = IF Bug = "held_refused" THEN "refused" ELSE "held"]
               /\ gate' = GateGone(p)
               /\ UNCHANGED <<lock, rwait>>
       ELSE IF rwait[p] < ReaderWait
          THEN /\ pc' = [pc EXCEPT ![p] = "readers"]
               /\ rwait' = [rwait EXCEPT ![p] = @ + 1]
               /\ gate' = IF Bug = "no_gate" THEN gate ELSE [gate EXCEPT ![g] = @ \cup {p}]
               /\ UNCHANGED lock
       ELSE /\ pc' = [pc EXCEPT ![p] = IF Bug = "reader_refused" THEN "refused" ELSE "failed"]
            /\ gate' = GateGone(p)
            /\ UNCHANGED <<lock, rwait>>
    /\ Unexcused(p)
    /\ UNCHANGED <<at, cur, shared, sockFile, sockAt, journal, view, seen, bound, hwait, sent, starts, reads, stops, deaths,
                   fails>>

\* A try of the wait for a lock a writer holds: a pause, then the socket asked.
HeldTick(p) ==
    /\ pc[p] = "held"
    /\ IF Answers(p)
          THEN /\ pc' = [pc EXCEPT ![p] = "refused"]
               /\ UNCHANGED <<hwait, rwait>>
          ELSE IF hwait[p] + 1 >= HeldWait
          THEN /\ pc' = [pc EXCEPT ![p] = "stuck"]
               /\ UNCHANGED <<hwait, rwait>>
          ELSE /\ pc' = [pc EXCEPT ![p] = "probed"]
               /\ hwait' = [hwait EXCEPT ![p] = @ + 1]
               /\ rwait' = [rwait EXCEPT ![p] = 0]
    /\ Unexcused(p)
    /\ UNCHANGED <<at, cur, lock, shared, gate, sockFile, sockAt, journal, view, seen, bound, sent, starts, reads,
                   stops, deaths, fails>>

Open(p) ==
    /\ pc[p] = OpenFrom
    /\ view' = [view EXCEPT ![p] = Len(journal[at[p]])]
    /\ seen' = [seen EXCEPT ![p] = SentIn(journal[at[p]])]
    /\ pc' = [pc EXCEPT ![p] = "open"]
    /\ UNCHANGED <<at, cur, lock, shared, gate, sockFile, sockAt, journal, bound, rwait, hwait, sent, starts, reads, stops,
                   deaths, fails, excused>>

Listen(p) ==
    /\ pc[p] = ListenFrom
    /\ IF Answers(p)
          THEN /\ pc' = [pc EXCEPT ![p] = "refused"]
               /\ UNCHANGED <<sockFile, sockAt, bound>>
          ELSE /\ sockFile' = [sockFile EXCEPT ![at[p]] = p]
               /\ sockAt' = [sockAt EXCEPT ![p] = at[p]]
               /\ bound' = [bound EXCEPT ![p] = TRUE]
               /\ pc' = [pc EXCEPT ![p] = ListenTo]
    /\ Unexcused(p)
    /\ UNCHANGED <<at, cur, lock, shared, gate, journal, view, seen, rwait, hwait, sent, starts, reads, stops, deaths,
                   fails>>

Restore(p) ==
    /\ pc[p] = RestoreFrom
    /\ journal' = [journal EXCEPT ![WG(p)] = Append(@, Record(p, IF seen[p] THEN "driver" ELSE "sent"))]
    /\ sent' = IF seen[p] THEN sent ELSE [sent EXCEPT ![WG(p)] = @ + 1]
    /\ seen' = [seen EXCEPT ![p] = TRUE]
    /\ view' = [view EXCEPT ![p] = @ + 1]
    /\ pc' = [pc EXCEPT ![p] = RestoreTo]
    /\ UNCHANGED <<at, cur, lock, shared, gate, sockFile, sockAt, bound, rwait, hwait, starts, reads, stops, deaths,
                   fails, excused>>

\* A step that fails instead (a store that does not read after its lock, a socket that cannot be bound, the restore's
\* commit refused): the start fails, exit 1 at its Exit, which releases the lock and removes its own socket.
FailAt(p, from) ==
    /\ pc[p] = from
    /\ fails < MaxFails
    /\ fails' = fails + 1
    /\ pc' = [pc EXCEPT ![p] = "failed"]
    /\ UNCHANGED <<at, cur, lock, shared, gate, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent,
                   starts, reads, stops, deaths, excused>>

OpenFail(p) == FailAt(p, OpenFrom)
ListenFail(p) == FailAt(p, ListenFrom)
RestoreFail(p) == FailAt(p, RestoreFrom)

Write(p) ==
    /\ pc[p] = "running"
    /\ Len(journal[WG(p)]) < MaxWrites
    /\ journal' = [journal EXCEPT ![WG(p)] = Append(@, Record(p, "step"))]
    /\ view' = [view EXCEPT ![p] = @ + 1]
    /\ UNCHANGED <<pc, at, cur, lock, shared, gate, sockFile, sockAt, seen, bound, rwait, hwait, sent, starts, reads,
                   stops, deaths, fails, excused>>

\* `nexus-server stop` (or SIGTERM) accepted: the loop ends and answers nothing more, its socket still bound. A start
\* refused while this server served was owed it, and is not owed another: the stop was asked for.
StopAccept(p) ==
    /\ pc[p] = "running"
    /\ stops < MaxStops
    /\ pc' = [pc EXCEPT ![p] = "stopping"]
    /\ stops' = stops + 1
    /\ excused' = [w \in Writers |-> excused[w] \/ pc[w] = "refused"]
    /\ UNCHANGED <<at, cur, lock, shared, gate, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent,
                   starts, reads, deaths, fails>>

\* The socket closed, and its file removed if the file in the server's own directory (held by its descriptor) is still
\* this server's; by the name (Bug "unlink_by_path"), whatever file the name names now.
CloseSocket(p) ==
    /\ pc[p] = "stopping"
    /\ sockFile' = IF Bug = "unlink_by_path" THEN [sockFile EXCEPT ![cur] = None]
                   ELSE IF sockFile[sockAt[p]] = p THEN [sockFile EXCEPT ![sockAt[p]] = None]
                   ELSE sockFile
    /\ pc' = [pc EXCEPT ![p] = "closed"]
    /\ UNCHANGED <<at, cur, lock, shared, gate, sockAt, journal, view, seen, bound, rwait, hwait, sent, starts, reads,
                   stops, deaths, fails, excused>>

\* The drivers ended, after the socket closed: their last record, under the lock, still held.
EndDrivers(p) ==
    /\ pc[p] = "closed"
    /\ journal' = [journal EXCEPT ![WG(p)] = Append(@, Record(p, "end"))]
    /\ view' = [view EXCEPT ![p] = @ + 1]
    /\ pc' = [pc EXCEPT ![p] = "ended"]
    /\ UNCHANGED <<at, cur, lock, shared, gate, sockFile, sockAt, seen, bound, rwait, hwait, sent, starts, reads, stops,
                   deaths, fails, excused>>

ReleaseAll(p) == [g \in Gens |-> lock[g] \ {p}]

\* The process ends, with the exit its state gives: 0 (refused, stopped) is not started again; 1 or 3 is, by the
\* service. A failed start removes its own socket's file, if it had one.
Exit(p) ==
    /\ pc[p] \in {"refused", "ended", "failed", "stuck"}
    /\ lock' = ReleaseAll(p)
    /\ pc' = [pc EXCEPT ![p] = IF pc[p] \in {"failed", "stuck"} THEN "crashed" ELSE "down"]
    /\ sockFile' = IF pc[p] = "failed" /\ bound[p] /\ sockFile[sockAt[p]] = p THEN [sockFile EXCEPT ![sockAt[p]] = None]
                                                                             ELSE sockFile
    /\ bound' = [bound EXCEPT ![p] = FALSE]
    /\ UNCHANGED <<at, cur, shared, gate, sockAt, journal, view, seen, rwait, hwait, sent, starts, reads, stops, deaths,
                   fails, excused>>

\* kill -9, or a crash, at any time: the system releases its lock and its gate; its socket's file stays, answering no
\* one.
Die(p) ==
    /\ pc[p] \notin {"down", "crashed"}
    /\ deaths < MaxDeaths
    /\ pc' = [pc EXCEPT ![p] = "crashed"]
    /\ lock' = ReleaseAll(p)
    /\ gate' = GateGone(p)
    /\ bound' = [bound EXCEPT ![p] = FALSE]
    /\ deaths' = deaths + 1
    /\ UNCHANGED <<at, cur, shared, sockFile, sockAt, journal, view, seen, rwait, hwait, sent, starts, reads, stops,
                   fails, excused>>

\* A reader: the lock shared when no writer holds it and no writer waits at the gate (else refused, having read
\* nothing), then let go. MaxReads 0: no bound on reads.
RLock(r) ==
    /\ pc[r] = "rdown"
    /\ MaxReads = 0 \/ reads[r] < MaxReads
    /\ reads' = IF MaxReads = 0 THEN reads ELSE [reads EXCEPT ![r] = @ + 1]
    /\ IF lock[cur] = {} /\ (Bug = "no_gate" \/ gate[cur] = {})
          THEN /\ shared' = [shared EXCEPT ![cur] = @ \cup {r}]
               /\ at' = [at EXCEPT ![r] = cur]
               /\ pc' = [pc EXCEPT ![r] = "rheld"]
          ELSE UNCHANGED <<shared, at, pc>>
    /\ UNCHANGED <<cur, lock, gate, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent, starts, stops,
                   deaths, fails, excused>>

RRelease(r) ==
    /\ pc[r] = "rheld"
    /\ shared' = [shared EXCEPT ![at[r]] = @ \ {r}]
    /\ pc' = [pc EXCEPT ![r] = "rdown"]
    /\ UNCHANGED <<at, cur, lock, gate, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent, starts, reads,
                   stops, deaths, fails, excused>>

\* The server's directory moved aside, and a new one (a store as the first was) made at its name.
MoveAside ==
    /\ cur < MaxMoves
    /\ cur' = cur + 1
    /\ UNCHANGED <<pc, at, lock, shared, gate, sockFile, sockAt, journal, view, seen, bound, rwait, hwait, sent, starts,
                   reads, stops, deaths, fails, excused>>

\* A writer's own steps, which it takes unless it dies: under weak fairness in Live.
Progress(p) == Restart(p) \/ Probe(p) \/ LockTry(p) \/ HeldTick(p) \/ Open(p) \/ Listen(p) \/ Restore(p) \/
               CloseSocket(p) \/ EndDrivers(p) \/ Exit(p)

\* Nothing left to do: every writer down with its starts spent, or serving with its writes, the stops and the deaths
\* spent; every reader done. The end of a behaviour, not a deadlock.
Spent(p) == \/ pc[p] = "down" /\ starts[p] = MaxStarts
            \/ pc[p] = "running" /\ Len(journal[WG(p)]) >= MaxWrites /\ stops = MaxStops /\ deaths = MaxDeaths
Finished == /\ \A p \in Writers : Spent(p)
            /\ \A r \in Readers : pc[r] = "rdown" /\ reads[r] = MaxReads
            /\ UNCHANGED vars

Next == \/ \E p \in Writers : Start(p) \/ Progress(p) \/ OpenFail(p) \/ ListenFail(p) \/ RestoreFail(p) \/ Write(p) \/
                              StopAccept(p) \/ Die(p)
        \/ \E r \in Readers : RLock(r) \/ RRelease(r)
        \/ MoveAside
        \/ Finished

Spec == Init /\ [][Next]_vars

\* The fairness, in plain words (models/store/README.md): every writer takes its own next step, and the service starts
\* again, without limit, every process that ended other than with exit 0 (Restart is in Progress); every reader lets go
\* (its copy's limit). A death, a stop, a failure, a start by a person, a write, a reader's lock and a move happen or
\* not.
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

\* A server whose socket is bound still has its socket's file: none was removed by another process.
SocketKept == \A q \in Writers : pc[q] \in Bound => sockFile[sockAt[q]] = q

\* A start is never left with nothing serving: once a writer is starting, some writer serves then or later. What
\* would have caught a start that exits 0 when a stopping server holds the lock, or when readers do not let go.
Serves == (\E p \in Writers : pc[p] = "start") ~> (\E q \in Writers : pc[q] = "running")

\* A start refused because a server was serving leads to a server serving, unless that server was then stopped on
\* purpose. What catches a start that took a stopping server's socket for a serving one (Bug "connect_probe").
RefusedServes == \A p \in Writers : (pc[p] = "refused" /\ ~excused[p]) ~>
                                    ((\E q \in Writers : pc[q] = "running") \/ excused[p])

\* No reader takes the lock while a writer waits on readers for that store: readers already in let go, and no stream
\* of new ones keeps the writer out (Bug "no_gate").
NoNewReaderWhileWriterWaits ==
    [][\A r \in Readers : (pc[r] = "rdown" /\ pc'[r] = "rheld") =>
                          ~\E p \in Writers : pc[p] = "readers" /\ at[p] = at'[r]]_vars

=============================================================================
