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

## StoreLock.tla: one writer, the start and the stop

`StoreLock.tla` is the store's lock (`Fs::lock` on the store's directory, taken first by `Store::open`; `docs/formats.md`, "One writer") and nexus-server's start and stop (`src/server/start_order.h`, `Server::run`), for several server processes (`Writers`) and readers (`Readers`: `nexus-server store check`) on one store, each of which may start, take its steps and die at any point (kill -9, a crash), after which the operating system has released its lock and its socket's file answers no one. The journal is a sequence of records, each with the number its writer gave it (one past what that writer replayed and wrote), whether its writer held that store's lock, and whether it had bound its socket in that run; what the journal does with them on disk is `Journal.tla`. The store starts with one session and one prompt queued. The store's directory may be moved aside with a new store made at its name (`MoveAside`); a process holds the directory it locked, by its descriptor, and its socket is in the same server directory.

| Action | The code |
|---|---|
| `Start(p)` | `nexus-server serve` started by a person: it holds the server directory at the name now (`Server::run`'s `dirFd_`), and everything it does is in that one |
| `Restart(p)` | the service starting it again after any exit but 0 (or a death) |
| `Probe(p)` | `Server::probeControl`: the socket asked `status`; a server that answers `SERVING` (one running, before it accepted `stop`), refused (exit 0) |
| `LockTry(p)` | one try of `PosixFs::lock(dir, Exclusive)`: the gate first (another writer holding it: `held`), then the lock: taken when free; held by a writer, the start waits (`held`); only readers, the gate kept, a pause and another try, `ReaderWait` times, then failed (exit 1) |
| `HeldTick(p)` | `StartOrder::Step::Wait`: a pause, the socket asked again: serving, refused; else the lock tried again, `HeldWait` tries in all, then stuck (exit 3) |
| `Open(p)` | `Store::open`, after the lock: the journal replayed; the process's view of it, and whether the queued prompt was sent |
| `Listen(p)` | `Server::listenControl`: the socket bound in the held directory (a stale file removed), or refused when a server says it serves |
| `Restore(p)` | `Server::restoreAll`: the sessions settled and the drivers restored; one record, which takes the queued prompt for a driver (sent, once) when this process's view still has it queued |
| `OpenFail(p)`, `ListenFail(p)`, `RestoreFail(p)` | the step fails instead (a store that does not read after its lock, a socket that cannot be bound, the restore's commit refused): failed, exit 1 at its `Exit` |
| `Write(p)` | a step of the running server, one record |
| `StopAccept(p)` | `nexus-server stop` (or SIGTERM) accepted: the loop ends and answers nothing more; the socket is still bound |
| `CloseSocket(p)` | the socket closed, and its file removed if the file in the held directory is still this server's (`unlinkOwnSocket`) |
| `EndDrivers(p)` | the drivers ended: their last record, under the lock, still held |
| `Exit(p)` | the process ends with the exit its state gives: 0 (refused, stopped) is not restarted, 1 and 3 are; a failed start removes its own socket's file |
| `Die(p)` | kill -9 or a crash, at any point: the lock and the gate released, the socket's file left |
| `RLock(r)`, `RRelease(r)` | a reader's shared lock (refused while a writer holds it, or a writer holds the gate), and its release (its copy done, or past its limit); `MaxReads` 0 is no bound on reads |
| `MoveAside` | the server's directory moved aside and a new store made at its name |

`Bug` selects the reversed witnesses' builds: `"unlocked"` the order before the lock (open, restore, listen, and no lock); `"restore_first"` the lock kept, the drivers restored before the socket; `"held_refused"` a lock held by a writer taken as another server, exit 0 at once (this order's first cut); `"reader_refused"` readers that did not let go taken as another server, exit 0; `"by_path"` writes made by the store's name rather than through the held directory; `"unlink_by_path"` a stop that removes whatever socket file its name names; `"connect_probe"` a connection taken as a server serving (the probe before `status`: a server restoring, or one that has accepted `stop`, "answers"); `"no_gate"` a reader takes the lock while a writer waits on readers.

`excused` is a ghost, held by no code: a start refused while a server served, whose server was then stopped on purpose (`StopAccept`), is not owed another server.

#### Fairness, in plain words

`Live` assumes two things, and the liveness properties (`Serves`, `RefusedServes`) rest on them:

1. Every writer takes its own next step, and every process that ends with anything but exit 0 (failed, stuck, killed) is started again, as often as it takes (`Restart` is one of the writer's own steps). This is a service manager that restarts on failure with no limit: launchd's `KeepAlive` with `SuccessfulExit` false (it throttles, never gives up), or systemd's `Restart=on-failure` with `StartLimitIntervalSec=0`.
2. Every reader lets go of its lock (its copy finishes, or its 500 ms limit makes it give up).

Not assumed: a person's start, a stop, a failure, a death, a write, a reader's lock, a move; each happens or not.

What does not meet them: a server a person starts by hand has no restart, so assumption 1 fails after its first exit 1 or 3, and nothing serves until the person starts it again (the exit line says why). systemd's default start limit (`StartLimitBurst=5` in `StartLimitIntervalSec=10s`) fails assumption 1 after five quick failures (a reader that does not let go fails each start in 2 s), and the unit stays `failed`. A reader that is suspended (stopped with a signal, its machine asleep) fails assumption 2 for as long as it is suspended. `docs/formats.md` says what the supervision layer sets.

| Property | What it says |
|---|---|
| `OneWriter` | at most one process has each store open |
| `OpenHoldsLock` | a process with a store open holds that store's lock, alone |
| `LockedWrites` | no record is written by a process that does not hold that store's lock |
| `ListenedWrites` | no record is written by a process that has not bound its socket in this run |
| `SeqsUnique` | the records are numbered 1, 2, 3, ... with no number twice (what `Journal.tla`'s recovery needs) |
| `SentOnce` | each store's queued prompt is written to a driver at most once |
| `Holders` | each lock has at most one writer, and never a writer and readers at once |
| `SocketKept` | a server whose socket is bound still has its socket's file: no other process removed it |
| `Serves` (under `Live`) | once a writer is starting, some writer serves then or later: a start is never left with nothing serving |
| `RefusedServes` (under `Live`) | a start refused (exit 0) and not excused leads to a server serving, or to its being excused (the server it found was stopped on purpose): no start exits 0 leaving nothing serving because it took a stopping server for a serving one |
| `NoNewReaderWhileWriterWaits` (action) | no reader takes a store's lock while a writer waits on readers for it: the writer waits only for the readers already in, however many come after |

| Config | Instance | Result |
|---|---|---|
| `StoreLock.cfg` | two servers and a reader, each server started up to twice, one stop, one death, up to three records; `Serves` and `RefusedServes` under `Live`, and `NoNewReaderWhileWriterWaits` | 126,888 distinct states, depth 46, 12 s on four workers |
| `StoreLockFail.cfg` | two servers, each started up to twice, one stop, one death, up to two failed steps (open, listen, restore); `Serves` and `RefusedServes` under `Live` | 137,707 distinct states, depth 48, 10 s |
| `StoreLockMove.cfg` | two servers, the directory moved aside once, one stop, one death; `Serves` under `Live` | 27,252 distinct states, depth 30, 1 s |
| `StoreLockThree.cfg` | three servers, each started up to twice, one stop, one death, up to three records; safety only | 1,388,174 distinct states, depth 47, 6 s on eight workers |
| `StoreLockGate.cfg` | two servers and two readers with no bound on reads; safety and `NoNewReaderWhileWriterWaits` (no liveness: a suspended reader is unbounded, `docs/formats.md`) | 3,562 distinct states, depth 35, under 1 s |

Reversed witnesses (in `witnesses/`), each counterexample read against the code and failing for the reason given:

| Witness | The break | Caught by | Counterexample |
|---|---|---|---|
| `LUnlockedTwoWriters` | the order before the lock | `OneWriter` | both start, both open |
| `LUnlockedWrites` | the order before the lock | `LockedWrites` | one starts, opens, restores: a record with no lock |
| `LUnlockedSeqs` | the order before the lock | `SeqsUnique` | both start, both open (both replay record 1), both restore: record 2 twice |
| `LUnlockedSentTwice` | the order before the lock | `SentOnce` | both start, both open with the prompt queued, both restore: the prompt sent twice (main's order: `openStore` restored the drivers, and only then did `listenControl` find the other server) |
| `LRestoreFirst` | the drivers restored before the socket | `ListenedWrites` | probe, lock, open, restore: a record before the socket is bound |
| `LByPath` | writes by the store's name | `LockedWrites` | a locks the store, opens, binds; the directory is moved aside; a's restore writes into the new store at the name, which it holds no lock on |
| `LUnlinkByPath` | the stop removes the file its name names | `SocketKept` | a runs; the directory is moved aside; b locks the new store and binds at the name; a's stop removes b's socket file while b listens |
| `LHeldRefused` | a held lock taken as another server (exit 0) | `Serves` | b runs; a starts; b is stopped (socket closed, drivers ending, lock held); a probes (no answer), finds the lock held, exits 0; b exits: nothing serves, nothing restarts |
| `LReaderRefused` | readers that do not let go taken as another server (exit 0) | `Serves` | a starts, probes; the reader takes the lock shared; a's tries outlast the wait and it exits 0; the reader lets go: nothing serves, nothing restarts |
| `LConnectProbe` | a connection taken as a server serving (the probe before `status`) | `RefusedServes` | a starts, serves; b starts; a accepts `stop` (socket still bound); b probes, connects, exits 0; a closes its socket, ends its drivers, exits: nothing serves, nothing restarts. The code before the fix: `probeControl` was a `connect` alone, and a stopping server keeps its socket until its loop ends (longer with one idle connection in its 2 s read) |
| `LNoGate` | no gate | `NoNewReaderWhileWriterWaits` | reader s takes the lock; a starts, probes, finds only readers and waits; reader r takes the lock while a waits. The code before the fix: `PosixFs::lock`'s retry on readers held nothing between its tries, so each new reader's shared lock was granted |

What is outside: a server from before the lock beside one of this build (it takes no lock, so nothing this build does can stop it writing); the code's `Probe` is for that case, and `docs/formats.md` states the window that is left during the upgrade.

### The binding

`tests/store/store_lock_walk.cpp` (`store_lock_walk <seed> <steps> [--faulty-unlocked | --faulty-restore-first | --faulty-held-refused | --faulty-reader-busy | --faulty-no-gate] <out.tla>`) is a functional test: two server processes and a reader on one real store, a directory `<out.tla>.store` in the test's own directory, each server with its own `PosixFs` (the real `flock` on the directory and the real gate, through the real `Store::open`) and the real `StartOrder` stepped by the real `take_step`, the dispatch `Server::run` takes its steps by; the reader takes `PosixFs::lock(dir, Shared)`, the lock `Store::inspect` takes first. The control socket is kept by the walk (whose file it is, and whether that server serves: running, before its stop was accepted). Time is a counter the walk advances by each pause: the waits are three retries on readers and three tries of a held lock (`ReaderWait` and `HeldWait` 3 in the trace instance), and during any pause on readers the reader may let go and, when it holds nothing, may try to start: the real `PosixFs::lock(dir, Shared)` against the waiting writer's real gate, which must refuse it (a reader that took the lock there is a step the spec cannot take). A seeded walk starts and restarts servers, takes each one's next step, writes, stops them (op `stopaccept`, then op `close`), exits and kills them now and then, and takes and releases the reader's lock. Each record is the step, with every process's state, how many hold the lock exclusive and shared (asked of the system with `flock` on a descriptor of the walk's own), whose the socket's file is, the record numbers in the log as they lie on the disk, and the sends of the queued prompt. `StoreLockTrace.tla` takes the `StoreLock` action each record names and requires the state it reaches to be the code's, so a step the spec cannot take, or takes elsewhere, has no successor; every safety property is checked on the trace. Seeds 1 to 5 run per commit, 2000 steps each (seed 1: 2,175 trace steps, 111 starts, 120 restarts, 144 locks, 78 waits on the reader, 64 waits on a held lock, 19 stops accepted, 108 deaths, 604 reader locks, 11 of them refused at a waiting writer's gate). The binding is shown to fail five times, each at the fault's step: `--faulty-unlocked` (`StartOrder::Fault::Unlocked`: an open with no lock before it), `--faulty-restore-first` (a restore before the socket), `--faulty-held-refused` (`StartOrder::Fault::HeldRefused`: a held lock refused at once), `--faulty-reader-busy` (the walk's filesystem reports readers that did not let go as a writer's lock: the start waits where the spec fails it), and `--faulty-no-gate` (`PosixOptions::test_no_gate`, the lock before the gate: a reader takes the lock while a writer waits on readers, seed 1's trace step 75). A fault the walk never reached is an error, so each rejection is the fault's.

Two parts of the start are bound by functional tests of the real `nexus-server` instead (`tests/link/server_lock.cpp`, `server_lock_functional`), as the walk keeps the socket itself: the question and its answer (`status`, `SERVING`), in `startDuringStop` (one idle connection holds the old server in its read, `stop` queues behind it, a start follows: it must not exit 0, and must serve once the old server ends; run against a probe that takes a connection alone as an answer, it fails: the start exits 0 and nothing serves); and the socket removed only by its identity in the held directory, in `dirMoved` (the old server, stopped through its moved socket, leaves the new server's socket at the old name).

## Running the checks

```sh
cmake --build build --target model_checks
TLA2TOOLS_JAR=/path/to/tla2tools.jar ctest --test-dir build -R tlc_journal -L '^model$' -j "$(sysctl -n hw.ncpu)"
```

`model` runs the two instances, the nine witnesses, five code traces (seeds 1 to 5, 1000 steps each) and the four faulty traces; `model_nightly` runs `JournalLarge`. `-R tlc_store_lock` runs StoreLock's five instances, eleven witnesses, five code traces and five faulty traces (CI shard `store-lock`). Each run has its own directory under `build/tests/store/tlc/<test>/` with its `tlc.log` (and the trace's `JournalTraceData.tla`), four TLC workers, and a 60 second cap (300 on the nightly). The runner is `tests/store/tlc.cmake`, which follows `tests/model/tlc.cmake`.
