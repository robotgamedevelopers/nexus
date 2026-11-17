# models/migration: old-layout migration and store format against rollback

`Migration.tla` models layer 5 of replacing a running server (`docs/roadmap.sexp`, `takeover-migration`; `docs/builds.md`
and `docs/formats.md` describe the rules; `src/server/migration.cpp`, `src/store/store.cpp` and `src/server/builds.cpp`
are the code):

1. **Old-layout migration (`~/.nexus` -> `~/.nexus/server`)**: server files left at the top of `~/.nexus` from before
   `~/.nexus/server` existed (`store/`, `drivers`, `config`, `server.log`, and a stale `server.sock`) move once into
   `~/.nexus/server` under `~/.nexus/server`'s exclusive OS lock (`flock`), refusing while an old server still answers on
   `~/.nexus/server.sock` or holds `~/.nexus/store`, refusing rather than overwriting an already-populated
   `~/.nexus/server/store` or differing `drivers`/`config`, and resuming from any process kill or machine crash without
   losing, tearing, duplicating or falsely refusing on resume.
2. **Store format against rollback**: when build `N+1` writes appended fields, unknown change kinds (including a framed
   record whose only change is an unknown kind on a held session) and unknown entry kinds with `reader <= Writes(N)`,
   rolling back to build `N` opens and replays the store without losing any session or entry; and when a build raises
   `reader` past `Writes(N)`, `builds back` and write-downgrading `builds current` (including multi-hop downgrades
   `5/5 -> 5/3 -> 3/3`) inspect the store and `nexus-config` on disk under the store's shared lock—allowing rollback
   when nothing incompatible was written (e.g. `N+1` failed before writing) and refusing when the store or config holds
   a version above `Writes(N)`.

## What is modelled

Variables:

| variable | what it is |
|---|---|
| `pc`, `curPart` | each actor's next migration phase (`Options::hook` names) and the part being staged or renamed |
| `migLock` | holder of `~/.nexus/server`'s exclusive OS lock, or `none` |
| `topNow`, `topDur`, `topSockNow`, `topSockDur` | `~/.nexus` server parts (`snap`, `log`, `drivers`, `config`) and `server.sock`, live and synced |
| `oldSrv` | old-layout server state: `"off"`, `"sock"` (answers `status` on `~/.nexus/server.sock`), or `"locked"` (holds `~/.nexus/store`) |
| `dirNow`, `dirDur`, `fileSynced` | `~/.nexus/server` files (`0` absent, `1` migrated from `top`, `2` pre-existing in `dir`, `3` torn after unsynced rename), live and synced |
| `stgNow`, `stgDur` | `.mig-<part>` staging files in `~/.nexus/server`, live and synced |
| `planNow`, `planDur` | `~/.nexus/server/.migrating` plan set, live and synced |
| `refusedConflict`, `migratedOnce` | whether a run refused a genuine `top`/`dir` conflict; ghost set once migration completed cleanly |
| `curBuild`, `prevBuild`, `srvUp` | active build pair (`1`: `3/3`, `2`: `5/3` appended/unknown, `3`: `5/5` breaking reader) and whether `curBuild`'s server holds `store` |
| `maxReader`, `baseEntries`, `unknownChgRec`, `appendedTail`, `openFailed` | store reader floor on disk, durable entry count, later-format journal contents, and whether `Store::open` failed |
| `runs`, `crashes`, `deaths`, `buildOps` | bounds |

Actions, one per step of the code:

- migration (`src/server/migration.cpp`): `MStart`, `MLock`, `MProbe`, `MPlan` (cleans `.migrating.tmp` / `.mig-*`, checks
  conflicts, commits `.migrating` or settles on resume), `MPlanSync` (syncs `.migrating`, `dir/store`, `dir` and `top`),
  `MStage`, `MSyncStg`, `MRename`, `MSyncPlaced`, `MCleanTop`, `MSyncTop`, `MFinish` (removes `.migrating` and syncs
  `dir`), `MUnlock`;
- environment: `OldStop` (old server stops), `Die` (process killed: OS lock released, page-cache writes stay), `Crash`
  (power cut: unsynced directory entries may revert or survive, and a renamed file whose staging was not synced may
  survive torn);
- builds and store (`src/server/builds.cpp`, `src/store/store.cpp`): `SwitchBuild`, `RollbackBuild`, `SrvOpen`,
  `SrvWrite`, `SrvCompact`, `SrvStop`.

Properties:

| property | kind | what it says |
|---|---|---|
| `TypeOK` | invariant | every variable stays in its domain |
| `LockHeld` | invariant | every migration step past `lock` is taken by `migLock`'s holder alone, and at most one actor is past `lock` |
| `LiveOldRefused` | invariant | while `oldSrv # "off"`, nothing in `~/.nexus` or `~/.nexus/server` is written or removed |
| `NeverClobbersNewStore` | invariant | when `~/.nexus` and `~/.nexus/server` start with conflicting data, neither is overwritten or removed |
| `OldDataNeverLost` | invariant | on an unconflicted layout, no file is ever torn, lost from both homes, or blocked by a false conflict after crash/kill |
| `MovesOnce` | invariant | a server never opens with old-layout files left behind, and once migrated `~/.nexus` stays clean forever |
| `CurAlwaysReadsStore` | invariant | `maxReader <= Writes(curBuild)` at every reachable state |
| `RollbackReadable` | invariant | rolling back from a compatible newer build opens cleanly (`~openFailed`) and loses no base session or entry |
| `CutAnywhereResumes` | liveness | under weak fairness (`OldStop`, `MStart`, `MigStep`), migration completes with every old part durable in `~/.nexus/server` |

## Instances

| config | what | when | result |
|---|---|---|---|
| `Migration.cfg` | two actors, `snap`, `log`, `drivers` and `server.sock`, live old server, crash + kill + build switch/rollback (`Live`) | per commit | pass: 15,350 distinct states, ~2.4 s on 4 workers |
| `MigrationConflict.cfg` | two actors with pre-existing `~/.nexus/server/store` alongside `~/.nexus/store`, crash + kill (`Live`) | per commit | pass: 46–268 distinct states, ~1.0 s on 4 workers |
| `MigrationRollback.cfg` | builds `1 (3/3)`, `2 (5/3)`, `3 (5/5)` across writes, unknown-change records, compactions, multi-hop switches and rollbacks (`Live`) | per commit | pass: 2,278 distinct states, ~1.1 s on 4 workers |
| `MigrationWide.cfg` | two actors, all four parts (`snap`, `log`, `drivers`, `config`), two crashes, two kills, builds `1..3` (`Live`) | nightly | pass: 72,813 distinct states, ~9.4 s on 8 workers |

## Reversed witnesses

Each config in `witnesses/` sets `Bug` to one break; TLC must report the invariant or temporal property named.

| witness | the break | property violated |
|---|---|---|
| `MNoLock` | migration runs without `~/.nexus/server`'s OS lock | `LockHeld` |
| `MIgnoreLiveOld` | migration proceeds while an old server answers on `server.sock` | `LiveOldRefused` |
| `MClobberNew` | migration overwrites a populated `~/.nexus/server/store` | `NeverClobbersNewStore` |
| `MNoSync` | staging files and directories are not synced before rename / old removal | `OldDataNeverLost` |
| `MNoPlan` | migration copies without committing `.migrating`, so a partial copy + crash falsely refuses as a conflict on resume | `OldDataNeverLost` |
| `MLeaveOld` | migration leaves old files behind in `~/.nexus` | `MovesOnce` |
| `MBlindBack` | `builds back` skips inspecting the store's reader floor on disk | `CurAlwaysReadsStore` |
| `MMultiHop` | `builds current` skips the disk reader check when downgrading `writes` (`5/5 -> 5/3 -> 3/3`) | `CurAlwaysReadsStore` |
| `MRefuseUnknownChange` | `Store::open` refuses a framed record on a held session whose only change is an unknown `Change::Kind` | `RollbackReadable` |
| `MStuckLockFile` | a dead migrator leaves a permanent lock file behind instead of an OS `flock` | `CutAnywhereResumes` |

## The code bound to the model

`MigrationTrace.tla` checks traces written by `tests/server/migration_walk.cpp` on real POSIX files, `flock` locks and
placed builds (`migration_walk <seed> <steps> [--fault <name>] <out.tla>`). Each trace is accepted when a behaviour of
`Migration.tla` (`Bug = "none"`) reaches its last record (`TraceUnfinished` violated), and rejected when none does (`pass`
with `CHECK_DEADLOCK FALSE`).

Accepted (`tlc_migration_trace_seed_1..4`, 300 steps each):
- seed 1: live old server on `server.sock`, then stop, concurrent actors, kills, crashes, switches and rollbacks;
- seed 2: live old process holding `flock(LOCK_EX)` on `~/.nexus/store`, then release, migration and build ops;
- seed 3: cold migration followed by build switches, compactions and rollbacks;
- seed 4: conflicting `~/.nexus/store` and `~/.nexus/server/store` refused without touching either store.

Rejected (`tlc_migration_trace_<fault>_rejected`): all nine code faults (`no-lock`, `ignore-live-old`, `clobber-new`,
`no-sync`, `no-plan`, `leave-old`, `blind-back`, `multihop`, `refuse-unknown-change`).

## Running

`ctest -L model -R '^tlc_migration'` (per commit) and `ctest -L model_nightly -R '^tlc_migration'` (nightly), through
`tests/store/tlc.cmake`; the walks need `migration_walk` built (`cmake --build build --target model_checks`).
