---------------------------- MODULE JournalTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real Journal         *)
(* (src/store/journal.cpp, on MemFs) is a behaviour of Journal.tla, with   *)
(* the code's own header sizes, one unit per byte.                         *)
(*                                                                         *)
(* tests/store/journal_walk.cpp drives the code with a seeded random walk *)
(* and writes JournalTraceData.tla beside this module. It defines          *)
(*   TraceRecLens, TraceSnapLens: the lengths of the records and           *)
(*     snapshots the walk wrote, and                                       *)
(*   Trace: one record per step: each filesystem call the journal made     *)
(*     (op create, append, truncate, sync, syncdir, rename, remove, with   *)
(*     the file's name and, for append and truncate, the byte count), the  *)
(*     recovery's decision (op recover: k, s, cut), each crash (op crash:  *)
(*     which pending directory changes it kept, and each changed file's    *)
(*     fate) and each acknowledgement (op ack: the record number); and     *)
(*     after each, the size of every file in the journal's directory and   *)
(*     the highest number acknowledged. Trace[1] is the journal just made  *)
(*     (op init).                                                          *)
(*                                                                         *)
(* Each step takes the Journal action the record names, with its          *)
(* arguments; TraceMatches requires the files' sizes, the recovery's       *)
(* decision and the acknowledgements to be the code's. A step the spec    *)
(* cannot take has no successor: TLC reports a deadlock. Every Journal    *)
(* invariant is checked on the trace too.                                  *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, JournalTraceData

VARIABLES data, dur, dnow, ddur, dpend, pc, seq, dseq, hist, whist, acked, writes, crashes, compacts, torn, wrong,
          broken, found, l

Unbounded == 1000000

J == INSTANCE Journal WITH Hdr <- 16, SnapHdr <- 24, RecLens <- TraceRecLens, SnapLens <- TraceSnapLens,
                           MaxWrites <- Unbounded, MaxCrashes <- Unbounded, MaxCompacts <- Unbounded, Bug <- "none"

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Sizes == [n \in {x \in J!Names : dnow[x] # 0} |-> Len(data[dnow[n]])]

Matches(k) ==
    LET r == Trace[k] IN
    /\ Sizes = r.sizes
    /\ acked = r.acked
    /\ r.op = "recover" => found = [k |-> r.k, s |-> r.s, cut |-> r.cut]

TraceMatches == Matches(l)

FateOf(fs) == [n \in J!Names |-> IF n \in DOMAIN fs THEN fs[n] ELSE J!NoFate]

Step(r) ==
    IF r.op = "append" THEN
        IF r.name = "log" THEN J!AppendRec(r.n)
        ELSE r.name = "snapshot.tmp" /\ J!AppendSnap(r.n)
    ELSE IF r.op = "sync" THEN J!SyncFile(r.name)
    ELSE IF r.op = "syncdir" THEN J!SyncDir
    ELSE IF r.op = "create" THEN
        IF r.name = "snapshot.tmp" THEN J!CreateSnapTmp
        ELSE r.name = "log.new" /\ J!CreateLogNew
    ELSE IF r.op = "rename" THEN
        IF r.name = "snapshot.tmp" THEN J!RenameSnap
        ELSE r.name = "log.new" /\ J!RenameLog
    ELSE IF r.op = "remove" THEN J!RemoveTmp(r.name)
    ELSE IF r.op = "truncate" THEN r.n = found.cut /\ J!Truncate
    ELSE IF r.op = "recover" THEN J!Recover
    ELSE IF r.op = "crash" THEN J!CrashWith(r.keep, FateOf(r.fates))
    ELSE IF r.op = "ack" THEN J!Ack(r.n)
    ELSE FALSE

traceVars == <<data, dur, dnow, ddur, dpend, pc, seq, dseq, hist, whist, acked, writes, crashes, compacts, torn, wrong,
               broken, found, l>>

TraceInit == l = 1 /\ J!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

TypeOK == J!TypeOK
NotBroken == J!NotBroken
NoTornAccepted == J!NoTornAccepted
RecoveredWritten == J!RecoveredWritten
NoAckedLost == J!NoAckedLost
CutExact == J!CutExact

=============================================================================
