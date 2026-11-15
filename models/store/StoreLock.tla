----------------------------- MODULE StoreLock -----------------------------
(***************************************************************************)
(* The store's lock and nexus-server's start-up order                      *)
(* (src/server/start_order.h, src/store/store.cpp Store::open,             *)
(* src/store/fs.h Fs::lock): several nexus-server processes, each of which *)
(* may start, take its steps, and die at any point, on one store.          *)
(*                                                                         *)
(* A process's start, in the code's order:                                 *)
(*   Probe    a server answers on the control socket: refused              *)
(*   Lock     the store's lock, exclusive, never waiting: refused when     *)
(*            another process holds it                                     *)
(*   Open     the replay: the process's view of the journal is what is     *)
(*            there now, and whether the queued prompt was sent            *)
(*   Listen   the control socket: refused when it is held, else bound     *)
(*   Restore  the drivers restored: one record, which marks the queued     *)
(*            prompt sent when this process's view says it is still        *)
(*            queued (a driver started for it, the prompt written once)    *)
(* then Write, a record per step of the running server, and Die at any     *)
(* time: kill -9, a crash or an exit, after which the operating system has *)
(* released the process's lock and nothing answers on its socket. A        *)
(* refused process only dies. The journal is a sequence of records, each   *)
(* with the number its writer gave it (one past its view), and whether its *)
(* writer held the lock and the socket when it wrote it.                   *)
(*                                                                         *)
(* What the journal does with its records (the log, the snapshot, torn     *)
(* tails, crashes of the machine) is Journal.tla; here a record is only    *)
(* its number and its writer. A server from before the lock (no lock at    *)
(* all) beside one of this build is outside the model: the code's Probe is *)
(* there for it, and the model keeps the step.                             *)
(*                                                                         *)
(* Bug selects a broken order for the reversed witnesses:                  *)
(*   "unlocked"      the order before the lock: open, restore, listen      *)
(*   "restore_first" the lock kept, the drivers restored before the socket *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Procs, MaxStarts, MaxWrites, Bug

ASSUME Bug \in {"none", "unlocked", "restore_first"}

VARIABLES pc, lock, sock, journal, view, seen, sent, starts

vars == <<pc, lock, sock, journal, view, seen, sent, starts>>

States == {"down", "start", "probed", "locked", "open", "listening", "restored", "running", "refused"}
\* A process that has the store open, for reading what it replayed and for writing.
Opened == {"open", "listening", "restored", "running"}

\* The record the store held before any of these processes: one session with a prompt queued.
First == [seq |-> 1, kind |-> "init", locked |-> TRUE, listened |-> TRUE]

SentIn(j) == \E i \in 1..Len(j) : j[i].kind = "sent"

TypeOK ==
    /\ pc \in [Procs -> States]
    /\ lock \subseteq Procs
    /\ sock \subseteq Procs
    /\ view \in [Procs -> Nat]
    /\ seen \in [Procs -> BOOLEAN]
    /\ sent \in Nat
    /\ starts \in [Procs -> 0..MaxStarts]

Init ==
    /\ pc = [p \in Procs |-> "down"]
    /\ lock = {}
    /\ sock = {}
    /\ journal = <<First>>
    /\ view = [p \in Procs |-> 0]
    /\ seen = [p \in Procs |-> FALSE]
    /\ sent = 0
    /\ starts = [p \in Procs |-> 0]

\* Where each step starts and ends, by the order Bug selects.
ProbeFrom == "start"
LockFrom  == "probed"
OpenFrom  == IF Bug = "unlocked" THEN "start" ELSE "locked"
ListenFrom == IF Bug = "none" THEN "open" ELSE "restored"
ListenTo  == IF Bug = "none" THEN "listening" ELSE "running"
RestoreFrom == IF Bug = "none" THEN "listening" ELSE "open"
RestoreTo == IF Bug = "none" THEN "running" ELSE "restored"

\* One record written by p, numbered one past what p replayed and wrote.
Record(p, kind) == [seq |-> view[p] + 1, kind |-> kind, locked |-> p \in lock, listened |-> p \in sock]

Start(p) ==
    /\ pc[p] = "down"
    /\ starts[p] < MaxStarts
    /\ pc' = [pc EXCEPT ![p] = "start"]
    /\ starts' = [starts EXCEPT ![p] = @ + 1]
    /\ UNCHANGED <<lock, sock, journal, view, seen, sent>>

Probe(p) ==
    /\ Bug # "unlocked"
    /\ pc[p] = ProbeFrom
    /\ pc' = [pc EXCEPT ![p] = IF sock = {} THEN "probed" ELSE "refused"]
    /\ UNCHANGED <<lock, sock, journal, view, seen, sent, starts>>

Lock(p) ==
    /\ Bug # "unlocked"
    /\ pc[p] = LockFrom
    /\ IF lock = {}
          THEN /\ lock' = {p}
               /\ pc' = [pc EXCEPT ![p] = "locked"]
          ELSE /\ pc' = [pc EXCEPT ![p] = "refused"]
               /\ UNCHANGED lock
    /\ UNCHANGED <<sock, journal, view, seen, sent, starts>>

Open(p) ==
    /\ pc[p] = OpenFrom
    /\ pc' = [pc EXCEPT ![p] = "open"]
    /\ view' = [view EXCEPT ![p] = Len(journal)]
    /\ seen' = [seen EXCEPT ![p] = SentIn(journal)]
    /\ UNCHANGED <<lock, sock, journal, sent, starts>>

Listen(p) ==
    /\ pc[p] = ListenFrom
    /\ IF sock = {}
          THEN /\ sock' = {p}
               /\ pc' = [pc EXCEPT ![p] = ListenTo]
          ELSE /\ pc' = [pc EXCEPT ![p] = "refused"]
               /\ UNCHANGED sock
    /\ UNCHANGED <<lock, journal, view, seen, sent, starts>>

Restore(p) ==
    /\ pc[p] = RestoreFrom
    /\ journal' = Append(journal, Record(p, IF seen[p] THEN "driver" ELSE "sent"))
    /\ sent' = IF seen[p] THEN sent ELSE sent + 1
    /\ seen' = [seen EXCEPT ![p] = TRUE]
    /\ view' = [view EXCEPT ![p] = @ + 1]
    /\ pc' = [pc EXCEPT ![p] = RestoreTo]
    /\ UNCHANGED <<lock, sock, starts>>

Write(p) ==
    /\ pc[p] = "running"
    /\ Len(journal) < MaxWrites
    /\ journal' = Append(journal, Record(p, "step"))
    /\ view' = [view EXCEPT ![p] = @ + 1]
    /\ UNCHANGED <<pc, lock, sock, seen, sent, starts>>

\* The process ends, however: the operating system releases its lock, and nothing answers on its socket.
Die(p) ==
    /\ pc[p] # "down"
    /\ pc' = [pc EXCEPT ![p] = "down"]
    /\ lock' = lock \ {p}
    /\ sock' = sock \ {p}
    /\ UNCHANGED <<journal, view, seen, sent, starts>>

\* Every process down and started as often as the instance allows: the end of a behaviour, not a deadlock.
Finished == (\A p \in Procs : pc[p] = "down" /\ starts[p] = MaxStarts) /\ UNCHANGED vars

Next == \/ \E p \in Procs : Start(p) \/ Probe(p) \/ Lock(p) \/ Open(p) \/ Listen(p) \/ Restore(p) \/ Write(p) \/ Die(p)
        \/ Finished

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
(* Properties *)

\* At most one process has the store open.
OneWriter == Cardinality({p \in Procs : pc[p] \in Opened}) <= 1

\* A process with the store open holds its lock.
OpenHoldsLock == \A p \in Procs : pc[p] \in Opened => lock = {p}

\* No record is written by a process that does not hold the lock.
LockedWrites == \A i \in 1..Len(journal) : journal[i].locked

\* Nothing is written before the socket is held as well (the drivers are restored only then).
ListenedWrites == \A i \in 1..Len(journal) : journal[i].listened

\* The records are numbered 1, 2, 3, ... with no number twice: what the journal's next open needs (Journal.tla).
SeqsUnique == \A i \in 1..Len(journal) : journal[i].seq = i

\* The queued prompt is written to a driver at most once.
SentOnce == sent <= 1

\* The lock and the socket each have at most one holder, and only a holder of the lock holds the socket.
Holders == Cardinality(lock) <= 1 /\ sock \subseteq lock

=============================================================================
