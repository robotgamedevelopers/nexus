------------------------------ MODULE Journal ------------------------------
(***************************************************************************)
(* The layout of the store's journal (src/store/journal.cpp): an          *)
(* append-only log of checksummed, numbered records, a snapshot that      *)
(* stands for the records up to its number, and the recovery that runs at *)
(* every open. The filesystem is modelled at the level the code relies on *)
(* (src/store/fs.h): files of units, each with the units a reader sees    *)
(* now and the units that survive a crash; a directory with its names now,*)
(* its names that survive, and the changes in between.                    *)
(*                                                                         *)
(* A unit is one byte in the code. A record of n units is the tuple       *)
(* <<1, s, i, n, w>> at its i-th unit: record number s, the write w that  *)
(* put it there (a ghost: two writes of one number differ, as their bytes *)
(* and checksums do). A snapshot's units are <<2, S, i, n, w>>. A damaged *)
(* unit is NoUnit: not what was written there. The reader's checksum is   *)
(* exact here: a record is whole when every one of its units is the one   *)
(* written (a CRC-32C collision is outside the model).                    *)
(*                                                                         *)
(* The index arithmetic is the code's: a record at offset o (1-based)     *)
(* spans o .. o + n - 1, the next one starts at o + n, the header is Hdr  *)
(* units and must be present before the length is read, and the cut is    *)
(* the offset after the last record kept.                                 *)
(*                                                                         *)
(* Each action is one filesystem call of the code, or the recovery's      *)
(* decision, or a crash, or the acknowledgement of a synced record. pc is *)
(* where the code is. Bug selects a broken variant for the reversed       *)
(* witnesses; "none" is the code.                                         *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Hdr,          \* units of a record's header (16 in the code)
    SnapHdr,      \* units of the snapshot's header (24 in the code)
    RecLens,      \* the lengths a record may have, header included
    SnapLens,     \* the lengths a snapshot may have, header included
    MaxWrites,    \* bound: records and snapshots written
    MaxCrashes,   \* bound: crashes
    MaxCompacts,  \* bound: compactions started
    Bug

ASSUME Hdr >= 1 /\ SnapHdr >= 1
ASSUME \A n \in RecLens : n > Hdr
ASSUME \A n \in SnapLens : n >= SnapHdr

Bugs == {"none", "accepttorn", "cuttoofar", "notruncate", "ackbeforesync", "renamebeforesync", "nodirsync",
         "compactnosync", "noskip", "truncoffbyone"}
ASSUME Bug \in Bugs

Names == {"log", "snapshot", "snapshot.tmp", "log.new"}
NIno == 6
Inos == 1..NIno
NoUnit == <<0, 0, 0, 0, 0>>
RecUnits(s, n, w) == [i \in 1..n |-> <<1, s, i, n, w>>]
SnapUnits(s, n, w) == [i \in 1..n |-> <<2, s, i, n, w>>]
\* How one file comes through a crash (MemFs::FileFate): revert to its durable units, or keep the units it shares
\* with them and `keep` more of those written since, with units bf+1 .. bt (offsets as the code's, 0-based [bf, bt))
\* damaged.
NoFate == [revert |-> TRUE, keep |-> 0, bf |-> 0, bt |-> 0]

VARIABLES
    data,      \* [Inos -> Seq(Unit)]: what a reader sees now
    dur,       \* [Inos -> Seq(Unit)]: what survives a crash
    dnow,      \* [Names -> 0..NIno]: the directory now (0: no such name)
    ddur,      \* the directory that survives a crash
    dpend,     \* Seq([from, to, ino]): the directory's changes since its last sync, in order
    pc,        \* where the code is
    seq,       \* the code's last record number (memory)
    dseq,      \* the code's last record number known durable (memory)
    \* ghosts
    hist,      \* the write of each record number, 1 .. seq, in the current history
    whist,     \* for each snapshot written (by its write), the history it stands for
    acked,     \* the highest record number acknowledged (survives every crash: it is what callers were told)
    writes, crashes, compacts,
    torn,      \* a recovery kept a record that was not whole
    wrong,     \* a recovery returned a history that was not the one written
    broken,    \* a recovery found the disk inconsistent and refused to open
    found      \* what the last recovery found: [k, s, cut]

vars == <<data, dur, dnow, ddur, dpend, pc, seq, dseq, hist, whist, acked, writes, crashes, compacts, torn, wrong,
          broken, found>>

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b
Last(s) == s[Len(s)]

Pcs == {"up", "open", "trunc", "truncsync", "c2", "c3", "c4", "c4b", "c5", "c6", "c7", "c8", "dead"}

-----------------------------------------------------------------------------
(* Files and directory *)

RefBy(dn, dd, dp, i) == (\E n \in Names : dn[n] = i \/ dd[n] = i) \/ (\E j \in 1..Len(dp) : dp[j].ino = i)
Free == {i \in Inos : ~RefBy(dnow, ddur, dpend, i)}
NewIno == CHOOSE i \in Free : \A j \in Free : i <= j
\* Files no name refers to any more are gone.
Clean(f, dn, dd, dp) == [i \in Inos |-> IF RefBy(dn, dd, dp, i) THEN f[i] ELSE <<>>]

Op1(d, op) == LET d1 == IF op.from # "" THEN [d EXCEPT ![op.from] = 0] ELSE d
              IN IF op.to # "" THEN [d1 EXCEPT ![op.to] = op.ino] ELSE d1
\* The surviving directory: its durable names with the kept changes applied in order.
RECURSIVE ApplyOps(_, _, _, _)
ApplyOps(d, dp, keep, j) ==
    IF j > Len(dp) THEN d ELSE ApplyOps(IF keep[j] THEN Op1(d, dp[j]) ELSE d, dp, keep, j + 1)

CommonPrefix(a, b) ==
    LET m == Min(Len(a), Len(b)) IN
    CHOOSE p \in 0..m : SubSeq(a, 1, p) = SubSeq(b, 1, p) /\ (p = m \/ a[p + 1] # b[p + 1])

Survive(d, du, fate) ==
    LET p == CommonPrefix(d, du) IN
    IF fate.revert THEN du
    ELSE LET e == p + Min(fate.keep, Len(d) - p)
         IN [j \in 1..e |-> IF j > Max(fate.bf, p) /\ j <= Min(fate.bt, e) THEN NoUnit ELSE d[j]]

\* Every fate a file may meet (for model checking; a trace names its own).
FatesFor(d, du) ==
    LET p == CommonPrefix(d, du)
        w == Len(d) - p
    IN {NoFate} \cup {[revert |-> FALSE, keep |-> k, bf |-> 0, bt |-> 0] : k \in 0..w}
       \cup {f \in [revert : {FALSE}, keep : 1..w, bf : p..(p + w), bt : p..(p + w)] :
               f.bf >= p /\ f.bf < f.bt /\ f.bt <= p + f.keep}

-----------------------------------------------------------------------------
(* The reader *)

\* The code's check of the record at offset o: a whole header, a length that fits, and the checksum (every unit as
\* written).
Whole(f, o) ==
    /\ o + Hdr - 1 <= Len(f)
    /\ f[o][1] = 1 /\ f[o][3] = 1
    /\ o + f[o][4] - 1 <= Len(f)
    /\ \A i \in 1..f[o][4] : f[o + i - 1] = <<1, f[o][2], i, f[o][4], f[o][5]>>
\* BROKEN (accepttorn): the checksum is not checked; the header and the length are.
HeaderFits(f, o) ==
    /\ o + Hdr - 1 <= Len(f)
    /\ f[o][1] = 1 /\ f[o][3] = 1
    /\ o + f[o][4] - 1 <= Len(f)
    /\ \A i \in 1..Hdr : f[o + i - 1] = <<1, f[o][2], i, f[o][4], f[o][5]>>
Reads(f, o) == IF Bug = "accepttorn" THEN HeaderFits(f, o) ELSE Whole(f, o)

\* The longest run of records the reader keeps from offset o, numbered one after another: each kept record's number,
\* write, offset and whether it was really whole; and the cut, the units kept.
RECURSIVE Chain(_, _, _, _)
Chain(f, o, prev, acc) ==
    IF Reads(f, o) /\ (acc = <<>> \/ f[o][2] = prev + 1)
    THEN Chain(f, o + f[o][4], f[o][2],
               Append(acc, [s |-> f[o][2], w |-> f[o][5], at |-> o, ok |-> Whole(f, o)]))
    ELSE [recs |-> acc, cut |-> o - 1]

SnapWhole(f) ==
    /\ Len(f) >= SnapHdr
    /\ f[1][1] = 2 /\ f[1][4] = Len(f)
    /\ \A i \in 1..Len(f) : f[i] = <<2, f[1][2], i, Len(f), f[1][5]>>

-----------------------------------------------------------------------------
Init ==
    /\ data = [i \in Inos |-> <<>>]
    /\ dur = [i \in Inos |-> <<>>]
    /\ dnow = [n \in Names |-> IF n = "log" THEN 1 ELSE 0]
    /\ ddur = dnow
    /\ dpend = <<>>
    /\ pc = "up"
    /\ seq = 0
    /\ dseq = 0
    /\ hist = <<>>
    /\ whist = <<>>
    /\ acked = 0
    /\ writes = 0
    /\ crashes = 0
    /\ compacts = 0
    /\ torn = FALSE
    /\ wrong = FALSE
    /\ broken = FALSE
    /\ found = [k |-> 0, s |-> 0, cut |-> 0]

ghosts == <<hist, whist, acked, writes, crashes, compacts, torn, wrong, broken, found>>
LogI == dnow["log"]

-----------------------------------------------------------------------------
(* Running: append, sync, acknowledge *)

\* AppendRec: a record of n units at the end of the log, not yet durable.
AppendRec(n) ==
    /\ pc = "up" /\ n \in RecLens /\ writes < MaxWrites
    /\ data' = [data EXCEPT ![LogI] = @ \o RecUnits(seq + 1, n, writes + 1)]
    /\ seq' = seq + 1
    /\ writes' = writes + 1
    /\ hist' = Append(hist, writes + 1)
    /\ UNCHANGED <<dur, dnow, ddur, dpend, pc, dseq, whist, acked, crashes, compacts, torn, wrong, broken, found>>

\* sync(name): the file's units are durable. The log while running (only when something is new) and after a cut;
\* the snapshot under its temporary name before the rename (BROKEN renamebeforesync: under its name, after).
SyncFile(n) ==
    /\ dnow[n] # 0
    /\ \/ n = "log" /\ pc = "up" /\ dseq # seq /\ dseq' = seq /\ pc' = pc
       \/ n = "log" /\ pc = "truncsync" /\ pc' = "up" /\ UNCHANGED dseq
       \/ n = "snapshot.tmp" /\ pc = "c3" /\ Bug # "renamebeforesync" /\ pc' = "c4" /\ UNCHANGED dseq
       \/ n = "snapshot" /\ pc = "c4b" /\ pc' = "c5" /\ UNCHANGED dseq
    /\ dur' = [dur EXCEPT ![dnow[n]] = data[dnow[n]]]
    /\ UNCHANGED <<data, dnow, ddur, dpend, seq, ghosts>>

\* The code acknowledges record a once sync has returned (BROKEN ackbeforesync: once it is appended).
Ack(a) ==
    /\ pc = "up"
    /\ a = IF Bug = "ackbeforesync" THEN seq ELSE dseq
    /\ a > acked
    /\ acked' = a
    /\ UNCHANGED <<data, dur, dnow, ddur, dpend, pc, seq, dseq, hist, whist, writes, crashes, compacts, torn, wrong,
                   broken, found>>

-----------------------------------------------------------------------------
(* Compaction: snapshot.tmp written, synced, renamed to snapshot, directory synced; then log.new created and renamed
   over log, directory synced. It starts only with the log synced (BROKEN compactnosync: without). *)

CreateSnapTmp ==
    /\ pc = "up" /\ compacts < MaxCompacts /\ writes < MaxWrites
    /\ dseq = seq \/ Bug = "compactnosync"
    /\ dnow["snapshot.tmp"] = 0
    /\ LET i == NewIno IN
       /\ dnow' = [dnow EXCEPT !["snapshot.tmp"] = i]
       /\ dpend' = Append(dpend, [from |-> "", to |-> "snapshot.tmp", ino |-> i])
       /\ data' = [data EXCEPT ![i] = <<>>]
       /\ dur' = [dur EXCEPT ![i] = <<>>]
    /\ pc' = "c2"
    /\ compacts' = compacts + 1
    /\ UNCHANGED <<ddur, seq, dseq, hist, whist, acked, writes, crashes, torn, wrong, broken, found>>

\* The snapshot: it stands for records 1 .. seq.
AppendSnap(n) ==
    /\ pc = "c2" /\ n \in SnapLens /\ writes < MaxWrites
    /\ data' = [data EXCEPT ![dnow["snapshot.tmp"]] = @ \o SnapUnits(seq, n, writes + 1)]
    /\ whist' = whist @@ ((writes + 1) :> hist)
    /\ writes' = writes + 1
    /\ pc' = "c3"
    /\ UNCHANGED <<dur, dnow, ddur, dpend, seq, dseq, hist, acked, crashes, compacts, torn, wrong, broken, found>>

RenameSnap ==
    /\ \/ pc = "c4"
       \/ pc = "c3" /\ Bug = "renamebeforesync"
    /\ dnow' = [dnow EXCEPT !["snapshot"] = dnow["snapshot.tmp"], !["snapshot.tmp"] = 0]
    /\ dpend' = Append(dpend, [from |-> "snapshot.tmp", to |-> "snapshot", ino |-> dnow["snapshot.tmp"]])
    /\ pc' = IF pc = "c3" THEN "c4b" ELSE "c5"
    /\ UNCHANGED <<data, dur, ddur, seq, dseq, ghosts>>

\* sync_dir: the directory's changes are durable. After the snapshot's rename (BROKEN nodirsync: skipped there), and
\* after the log's.
SyncDir ==
    /\ pc \in {"c5", "c8"}
    /\ ddur' = dnow
    /\ dpend' = <<>>
    /\ data' = Clean(data, dnow, dnow, <<>>)
    /\ dur' = Clean(dur, dnow, dnow, <<>>)
    /\ pc' = IF pc = "c5" THEN "c6" ELSE "up"
    /\ UNCHANGED <<dnow, seq, dseq, ghosts>>

CreateLogNew ==
    /\ \/ pc = "c6"
       \/ pc = "c5" /\ Bug = "nodirsync"
    /\ dnow["log.new"] = 0
    /\ LET i == NewIno IN
       /\ dnow' = [dnow EXCEPT !["log.new"] = i]
       /\ dpend' = Append(dpend, [from |-> "", to |-> "log.new", ino |-> i])
       /\ data' = [data EXCEPT ![i] = <<>>]
       /\ dur' = [dur EXCEPT ![i] = <<>>]
    /\ pc' = "c7"
    /\ UNCHANGED <<ddur, seq, dseq, ghosts>>

RenameLog ==
    /\ pc = "c7"
    /\ dnow' = [dnow EXCEPT !["log"] = dnow["log.new"], !["log.new"] = 0]
    /\ dpend' = Append(dpend, [from |-> "log.new", to |-> "log", ino |-> dnow["log.new"]])
    /\ pc' = "c8"
    /\ UNCHANGED <<data, dur, ddur, seq, dseq, ghosts>>

-----------------------------------------------------------------------------
(* Crash and recovery *)

\* A crash at any point: the directory keeps the kept changes, each file its fate; memory is lost. The process starts
\* again at once.
CrashWith(keep, fate) ==
    /\ pc # "dead" /\ crashes < MaxCrashes
    /\ DOMAIN keep = 1..Len(dpend)
    /\ LET nd == ApplyOps(ddur, dpend, keep, 1)
           nf == [i \in Inos |-> IF \E n \in Names : nd[n] = i
                                 THEN Survive(data[i], dur[i], fate[CHOOSE n \in Names : nd[n] = i])
                                 ELSE <<>>]
       IN /\ dnow' = nd /\ ddur' = nd /\ dpend' = <<>>
          /\ data' = nf /\ dur' = nf
    /\ pc' = "open"
    /\ seq' = 0 /\ dseq' = 0
    /\ crashes' = crashes + 1
    /\ UNCHANGED <<hist, whist, acked, writes, compacts, torn, wrong, broken, found>>

FS(nd, n) == IF nd[n] = 0 THEN {NoFate} ELSE FatesFor(data[nd[n]], dur[nd[n]])
Crash ==
    \E keep \in [1..Len(dpend) -> BOOLEAN] :
        LET nd == ApplyOps(ddur, dpend, keep, 1) IN
        \E f1 \in FS(nd, "log"), f2 \in FS(nd, "snapshot"), f3 \in FS(nd, "snapshot.tmp"), f4 \in FS(nd, "log.new") :
            CrashWith(keep, "log" :> f1 @@ "snapshot" :> f2 @@ "snapshot.tmp" :> f3 @@ "log.new" :> f4)

\* Open, first: the temporaries of a compaction that did not finish go (snapshot.tmp, then log.new).
RemoveTmp(n) ==
    /\ pc = "open"
    /\ n \in {"snapshot.tmp", "log.new"} /\ dnow[n] # 0
    /\ n = "log.new" => dnow["snapshot.tmp"] = 0
    /\ dnow' = [dnow EXCEPT ![n] = 0]
    /\ dpend' = Append(dpend, [from |-> n, to |-> "", ino |-> 0])
    /\ UNCHANGED <<data, dur, ddur, pc, seq, dseq, ghosts>>

\* Open, then: read the snapshot and the log, keep the longest run of whole records numbered one after another, skip
\* what the snapshot stands for, and decide where the log is cut. A damaged snapshot, a missing log, a log that ends
\* before the snapshot's number, or a gap after it is a disk the code refuses to open (broken).
Recover ==
    /\ pc = "open" /\ dnow["snapshot.tmp"] = 0 /\ dnow["log.new"] = 0
    /\ LET hasSnap == dnow["snapshot"] # 0
           sf == IF hasSnap THEN data[dnow["snapshot"]] ELSE <<>>
           snapOK == ~hasSnap \/ SnapWhole(sf)
           S == IF hasSnap /\ snapOK THEN sf[1][2] ELSE 0
           sh == IF hasSnap /\ snapOK THEN whist[sf[1][5]] ELSE <<>>
           hasLog == dnow["log"] # 0
           lf == IF hasLog THEN data[dnow["log"]] ELSE <<>>
           ch == Chain(lf, 1, 0, <<>>)
           \* BROKEN (cuttoofar): a torn tail takes the last whole record with it
           trim == Bug = "cuttoofar" /\ ch.cut < Len(lf) /\ ch.recs # <<>>
           recs == IF trim THEN SubSeq(ch.recs, 1, Len(ch.recs) - 1) ELSE ch.recs
           cut == IF trim THEN Last(ch.recs).at - 1 ELSE ch.cut
           \* BROKEN (noskip): the records the snapshot stands for are replayed again, and nothing is checked
           after == IF Bug = "noskip" THEN recs ELSE SelectSeq(recs, LAMBDA r : r.s > S)
           bad == \/ ~snapOK \/ ~hasLog
                  \/ Bug # "noskip" /\ recs # <<>> /\ Last(recs).s < S
                  \/ Bug # "noskip" /\ after # <<>> /\ after[1].s # S + 1
           k == IF after = <<>> THEN S ELSE Last(after).s
           rh == sh \o [j \in 1..Len(after) |-> after[j].w]
       IN IF bad
          THEN /\ broken' = TRUE
               /\ pc' = "dead"
               /\ UNCHANGED <<seq, dseq, hist, torn, wrong, found>>
          ELSE /\ seq' = k
               /\ dseq' = k
               /\ found' = [k |-> k, s |-> S, cut |-> cut]
               /\ torn' = (torn \/ \E j \in 1..Len(recs) : ~recs[j].ok)
               /\ wrong' = (wrong \/ IF Len(rh) > Len(hist) THEN TRUE ELSE rh # SubSeq(hist, 1, Len(rh)))
               /\ hist' = rh
               \* BROKEN (notruncate): the torn tail stays and the next record goes after it
               /\ pc' = IF cut < Len(lf) /\ Bug # "notruncate" THEN "trunc" ELSE "up"
               /\ UNCHANGED broken
    /\ UNCHANGED <<data, dur, dnow, ddur, dpend, whist, acked, writes, crashes, compacts>>

\* Open, last: the log is cut where the run ends, then synced (SyncFile("log") in "truncsync"). BROKEN
\* (truncoffbyone): one unit of the torn tail stays.
Truncate ==
    /\ pc = "trunc"
    /\ data' = [data EXCEPT ![LogI] = SubSeq(@, 1, IF Bug = "truncoffbyone" THEN found.cut + 1 ELSE found.cut)]
    /\ pc' = "truncsync"
    /\ UNCHANGED <<dur, dnow, ddur, dpend, seq, dseq, ghosts>>

\* Running with nothing to do (the bounds reached): a state TLC may stop in. Anywhere else, a state with no step is a
\* deadlock TLC reports: recovery and compaction always go on.
Idle == pc = "up" /\ UNCHANGED vars

-----------------------------------------------------------------------------
Next ==
    \/ \E n \in RecLens : AppendRec(n)
    \/ \E n \in Names : SyncFile(n)
    \/ \E a \in 1..MaxWrites : Ack(a)
    \/ CreateSnapTmp
    \/ \E n \in SnapLens : AppendSnap(n)
    \/ RenameSnap \/ SyncDir \/ CreateLogNew \/ RenameLog
    \/ Crash
    \/ \E n \in {"snapshot.tmp", "log.new"} : RemoveTmp(n)
    \/ Recover \/ Truncate
    \/ Idle

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties *)

TypeOK ==
    /\ pc \in Pcs
    /\ dnow \in [Names -> 0..NIno] /\ ddur \in [Names -> 0..NIno]
    /\ seq \in Nat /\ dseq \in 0..seq /\ acked \in Nat
    /\ Len(hist) = seq \/ pc \in {"open", "dead"}
    /\ torn \in BOOLEAN /\ wrong \in BOOLEAN /\ broken \in BOOLEAN

\* The disk is never one the code must refuse: every crash leaves a snapshot that is whole (or none), a log, and
\* records that run on from the snapshot.
NotBroken == ~broken

\* No record that was not whole is ever kept.
NoTornAccepted == ~torn

\* What recovery returns is exactly what was written, in order: a prefix of the history the crash interrupted.
RecoveredWritten == ~wrong

\* No acknowledged record is lost: once open, the code holds every record it acknowledged.
NoAckedLost == pc \notin {"open", "dead"} => seq >= acked

\* The cut keeps every whole record of the run: after a recovery the log is the run and nothing after it, once
\* truncated (with the cut known, the units past it are the torn tail).
CutExact == pc = "truncsync" => Len(data[LogI]) = found.cut

=============================================================================
