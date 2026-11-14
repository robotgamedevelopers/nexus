# models/store: the journal under the server's durable state

`Journal.tla` is the layout of the journal in `src/store/journal.cpp`, the one structure on disk the server's state rests on: an append-only log of checksummed, numbered records, a snapshot that stands for the records up to its number, the compaction that writes the snapshot and starts the log again, and the recovery that runs at every open. The store (`src/store/store.cpp`) keeps one journal, and every durable step is one record: the changes one action makes to one tab (entries appended, a prompt queued) together with the number of that action in its window's list, so a tab and the window's applied counter can never disagree after a crash. nexus-server commits each action before the state that shows it is sent, so the window's acknowledgement implies the record was acknowledged here (`NoAckedLost`).

How the store joins the link: `models/Link.tla` already lets the server be given any range of a window's list, including one from its first action again; that is what a window does after a server restart, when it opens a new session and sends its actions again under the same numbers. The server's Applier starts that session from the counter the store kept, so `ExactlyOnce` across a restart rests on two things checked here and there: the counter and the tabs come back exactly as committed (this model, and `durable_test`'s crash at every write and sync), and the Applier skips what the counter covers (`Link.tla`).

## What is modelled

The filesystem at the level the code relies on (`src/store/fs.h`): each file has the units a reader sees now and the units that survive a crash; the directory has its names now, its names that survive, and the creates, renames and removes in between, in order. A crash keeps any subset of the directory's pending changes, applied in order, and gives each file a fate: its durable units, or those plus any number of the units written since, with any run of them damaged (a torn write). `MemFs` in the code implements exactly these crashes, and the unit tests and the trace walk crash it.

A unit is one byte in the code. A record of n units is `<<1, s, i, n, w>>` at its i-th unit (record number s, the write w that put it there, a ghost so that two writes of one number differ as their checksums do); a snapshot's are `<<2, S, i, n, w>>`; a damaged unit is `NoUnit`. The reader's checksum is exact in the model: a record is whole when every unit is the one written (a CRC-32C collision is outside the model). The index arithmetic is the code's: a record at offset o spans o .. o + n - 1, its header (`Hdr` units) must be present before its length is read, and the cut is the offset after the last record kept.

Each action is one filesystem call of the code, the recovery's decision, a crash, or an acknowledgement:

| Action | The code |
|---|---|
| `AppendRec(n)` | `Journal::sync`'s one write of every record appended since the last sync (`Journal::append` only keeps the record in memory, and a crash before the write loses it, which the spec allows): one `AppendRec` per record in the write |
| `SyncFile("log")` | `Journal::sync`, after the write, on the log it keeps open; the record numbers up to `seq` are durable |
| `Ack(a)` | the caller is told record a is durable (only after the sync returns) |
| `CreateSnapTmp`, `AppendSnap(n)`, `SyncFile("snapshot.tmp")`, `RenameSnap`, `SyncDir` | `Journal::compact`, first half: the snapshot written under a temporary name, synced, renamed, the directory synced (it starts only with the log synced) |
| `CreateLogNew`, `RenameLog`, `SyncDir` | compaction, second half: an empty log.new renamed over the log, the directory synced |
| `Crash` / `CrashWith(keep, fate)` | the machine stops anywhere; memory is lost |
| `RemoveTmp(n)` | `Journal::open`, first: the temporaries of an unfinished compaction go |
| `Recover` | `Journal::open`: the snapshot checked, the longest run of whole records numbered one after another, those the snapshot stands for skipped, the decision `found = [k, s, cut]` |
| `Truncate`, `SyncFile("log")` | `Journal::open`, last: the torn tail cut off and the cut synced, so the next record follows cleanly |

`Bug` selects a broken variant for the reversed witnesses; `"none"` is the code.

### Properties

| Property | What it says |
|---|---|
| `TypeOK` | every variable has its type |
| `NotBroken` | no crash ever leaves a disk the code must refuse: a whole snapshot or none, a log, and records that run on from the snapshot |
| `NoTornAccepted` | no record that was not whole is ever kept |
| `RecoveredWritten` | what recovery returns is exactly what was written, in order: a prefix of the history the crash interrupted |
| `NoAckedLost` | once open, the journal holds every record it acknowledged |
| `CutExact` | after the cut the log is the run of whole records and nothing after it |

### Instances

| Config | Instance | Result |
|---|---|---|
| `Journal.cfg` | header 2 units, records of 3, snapshot of 2, up to 4 writes, 3 crashes, 2 compactions | 309,507 distinct states, depth 27, 13 s on four workers |
| `JournalLens.cfg` | records of 3 or 4 units, 3 writes, 3 crashes, 1 compaction | 172,606 distinct states, depth 19, 7 s on four workers |
| `JournalLarge.cfg` (nightly) | records of 3 or 4 units, 4 writes, 3 crashes, 2 compactions | 2,358,570 distinct states, depth 27, 54 s on eight workers |

Capacity four (four records or snapshots written) is where a torn tail after a compaction, a crash inside the next compaction and a second recovery all fit in one behaviour.

## Reversed witnesses

Each config in `witnesses/` sets `Bug` to one broken variant on a small instance (header 1 unit) and checks only the property that must catch it. The test passes only when TLC reports that property violated; each counterexample was read and fails for the reason given.

| Witness | The break | Caught by | Counterexample |
|---|---|---|---|
| `JAcceptTorn` | the reader checks the header and the length, not the checksum | `NoTornAccepted` | append, crash tearing the record, recover keeps it |
| `JCutTooFar` | a torn tail takes the last whole record with it | `NoAckedLost` | append, sync, append, ack, crash tearing the second, recover cuts the first |
| `JNoTruncate` | the torn tail stays and the next record goes after it | `NoAckedLost` | torn tail kept, append, sync, ack, crash, recover stops at the garbage |
| `JAckBeforeSync` | a record is acknowledged once appended | `NoAckedLost` | append, ack, crash, recover |
| `JNoDirSync` | compaction skips the directory sync after the snapshot's rename | `NoAckedLost` | append, sync, ack, snapshot written, synced, renamed, the log replaced while the snapshot's name is not yet durable; crash keeps the new log and loses the snapshot, so record 1 is gone |
| `JRenameBeforeSync` | the snapshot is renamed before it is synced | `NotBroken` | snapshot written, renamed, crash tears it, recover finds it damaged |
| `JCompactNoSync` | compaction starts with records not yet synced | `NotBroken` | two appends, snapshot of both, crash loses the log's records, recover finds the log behind the snapshot |
| `JNoSkip` | records the snapshot stands for are replayed again | `RecoveredWritten` | append, sync, a snapshot of record 1, rename, crash, recover returns record 1 twice (from the snapshot and from the log) |
| `JTruncOffByOne` | the cut leaves one unit of the torn tail | `CutExact` | crash tears a record, recover, truncate one past the cut |

## The binding: code traces checked against the spec

`tests/store/journal_walk.cpp` drives the real `Journal` on a `MemFs` with a seeded random walk (`journal_walk <seed> <steps> [--fault accept-torn|cut-too-far|no-truncate|no-sync] <out.tla>`): appends of 1 to 8 bytes, syncs, compactions with snapshots of 0 to 8 bytes, crashes armed at the batch's write or its flush, or at any of the next nine filesystem calls of a compaction, and crashes inside open as well. Each crash takes a plan drawn from the seed: which of the directory's pending changes survive and, for every file changed, revert or keep a prefix with a damaged run. The walk records every successful filesystem call (its name and length; a write to the log also with each record's length, as one write holds every record since the last sync), each recovery's decision (`Journal::Observer`, called before the decision is checked, so a faulty decision shows even when the disk it leaves is then refused), each crash's plan, and each acknowledgement, with the size of every file and the highest number acknowledged after each step. It writes `JournalTraceData.tla`; seed 1 at 1000 steps gives 1,723 trace steps with 214 crashes, 19 torn tails cut and 74 compactions.

`JournalTrace.tla` instantiates `Journal` with the code's sizes (`Hdr` 16, `SnapHdr` 24, the lengths the walk wrote) and takes, at each trace step, the action the record names with its arguments (a write to the log is that many `AppendRec` steps, one per record, with the sizes matched after the last); `TraceMatches` requires the files' sizes, the recovery's decision and the acknowledgements to be the code's, and every `Journal` invariant is checked on the trace. A step the spec cannot take has no successor and TLC reports a deadlock.

The binding is shown to fail four times. `Journal::Fault` builds a faulty journal (test-only, never set by the store): `accept-torn` keeps a record whose checksum fails and `cut-too-far` drops the last whole record with a torn tail, and TLC reports `TraceMatches` violated at that recovery; `no-truncate` leaves the torn tail and `no-sync` writes the batch and acknowledges it without syncing (a write acknowledged before it is durable), and the spec cannot take the next step (deadlock). A faulty journal may leave a disk its own open refuses; the walk stops there and the trace up to it goes to TLC.

## Running the checks

```sh
cmake --build build --target model_checks
TLA2TOOLS_JAR=/path/to/tla2tools.jar ctest --test-dir build -R tlc_journal -L '^model$' -j "$(sysctl -n hw.ncpu)"
```

`model` runs the two instances, the nine witnesses, five code traces (seeds 1 to 5, 1000 steps each) and the four faulty traces; `model_nightly` runs `JournalLarge`. Each run has its own directory under `build/tests/store/tlc/<test>/` with its `tlc.log` (and the trace's `JournalTraceData.tla`), four TLC workers, and a 60 second cap (300 on the nightly). The runner is `tests/store/tlc.cmake`, which follows `tests/model/tlc.cmake`.
