# models/handover: durable, resumable build handover

`Handover.tla` models layer 3 of replacing a running server with a placed build (`docs/roadmap.sexp`,
`takeover-handover`; `docs/builds.md` says what the code does, `src/server/handover.cpp` and `src/server/drain.cpp` are
the code). Layer 1 (`models/store/StoreLock.tla`) keeps at most one writer on the store; layer 2
(`models/placement/Placement.tla`) places immutable builds and switches `(current, previous)` in one rename. Layer 3
sits on top of both: one handover runs at a time under `handover/`'s exclusive `flock`, writes durable synced stage
records (`handover/state`) before acting, drains the running server's open turns up to its deadline, rolls back to
`previous` when a target build fails to reach `SERVING`, and resumes deterministically from what is on disk after any
cut.

## What is modelled

`<server dir>/handover` (`hf`, `hfd`, `hdir`, `hdirD`, `hpend`, `lock`), `builds/active`'s `(cur, prev)` pair, and the
running server `srv` (`[b, draining, turn, late]`, where `LegacyBuild = 9` stands for a server from before `drain`).

Variables:

| variable | what it is |
|---|---|
| `pc`, `verb`, `tgt`, `rtgt`, `rprev`, `didOwn`, `wf` | each actor's next step (matching `Options::hook` in C++) and its command's locals |
| `lock` | holder of `handover/`'s exclusive `flock`, or `None` |
| `cur`, `prev` | `builds/active` current and previous build |
| `hf`, `hfd` | files in `handover/`, now and as last `fsync`'d |
| `hdir`, `hdirD`, `hpend` | directory entries in `handover/`, now, as last `fsync`'d, and pending changes since |
| `srv` | running server: `b` (`0` down, `1..3`, or `LegacyBuild`), `draining`, `turn` (open turn), `late` (drain deadline passed) |
| `ops`, `crashes`, `deaths`, `turns` | bounds |

Actions, one per step of `Handover::Run` (`Options::hook`):

- lock and recovery setup: `Start`, `LockTry`, `LockGiveUp` (exit 4), `Settle` (`sync_dir("handover")`), `CleanOne`
  (`.state-*` removed), `Inspect` (resumes any unfinished `handover/state` first so `previous` is never overwritten by
  a broken build; otherwise checks target and running server), `Unlock`;
- stage transitions (`WriteState` through `.state-*` + `rename` -> `state`, then `SyncState` on `handover/`):
  `wswitch`/`sswitch` -> `Switch`, `wdrain`/`sdrain` -> `DrainStep`, `wstart`/`sstart` -> `StartStep`,
  `wrollback`/`srollback` -> `RollbackSwitch` -> `StartRollback`, and `ClearState`/`SyncClear`;
- server and environment: `TurnOpen`, `TurnEnd`, `DrainLate`, `SrvDrainExit`, `Die` (`SIGKILL`: OS lock released, disk
  unchanged), `Crash` (all processes end, server down, unsynced bytes/directory ops subsetted).

Properties:

| property | kind | what it says |
|---|---|---|
| `TypeOK` | invariant | all state variables stay in their domains |
| `OneHandover` | invariant | at most one actor is past `"lock"`, and that actor holds `lock` |
| `CurrentTested` | invariant | `cur` is always in `Tested` |
| `NeverServesBroken` | invariant | `srv.b` is never in `Broken` |
| `StateValid` | invariant | every `handover/state` on disk is whole, names a `Tested` target, and keeps a working rollback build (`(Tested \ Broken) \cup {0}`) |
| `IdleCurrentWorks` | invariant | whenever no handover is active and `~HasState`, `cur` is a working tested build |
| `DrainCutsOnlyLate` | action | a non-legacy server with an open turn stops for handover only once its drain deadline has passed |
| `StopOnlyLegacy` | action | an non-draining server stops without a crash only when it is `LegacyBuild` |
| `HandoverEnds` | temporal | every handover or resume begun returns to `"idle"` |
| `AlwaysRecoversToServing` | temporal | cut anywhere (`Die` or `Crash`, target working or broken, turn open or not), `handover resume` brings a working tested build to `SERVING`, clears `handover/state`, and leaves `cur = srv.b` |

## Instances

| config | what | when | result |
|---|---|---|---|
| `Handover.cfg` | two actors, builds `{1, 2, 3}`, tested `{1, 2}`, broken `{2}`, death + crash + open turn | per commit | pass: 8,255 distinct states, 4 s on 4 workers |
| `HandoverCutResume.cfg` | `a1` hands over to any build and may die or crash anywhere; `a2` runs `resume` (`AlwaysRecoversToServing`) | per commit | pass: 1,594 distinct states, 3 s on 4 workers |
| `HandoverLegacy.cfg` | starts from `LegacyBuild` serving with open turns, death and crash | per commit | pass: 1,764 distinct states, 5 s on 4 workers |
| `HandoverLarge.cfg` | two actors, two commands each, two crashes, two deaths, two turns | nightly | pass: 89,535 distinct states, 7 s on 8 workers |

## Reversed witnesses

Each config in `witnesses/` enables one break; TLC must reject it with the property named:

| witness | the break | property | states |
|---|---|---|---|
| `HNoLock` | `handover/`'s `flock` is skipped | `OneHandover` | 9 |
| `HNoSync` | `.state-*` and `handover/` are not synced before acting | `IdleCurrentWorks` | 45 |
| `HInMemoryOnly` | stages are kept in memory without `handover/state` | `IdleCurrentWorks` | 21 |
| `HOverwritePrev` | a new `handover <hash>` overwrites an unfinished `handover/state` instead of resuming it first | `StateValid` | 794 |
| `HNoRollback` | a failed target build clears `handover/state` without rolling back to `previous` | `IdleCurrentWorks` | 29 |
| `HStopCutsTurn` | non-legacy server stopped immediately instead of draining open turns | `DrainCutsOnlyLate` | 76 |
| `HStopIdleNonLegacy` | non-legacy server stopped without `drain` | `StopOnlyLegacy` | 20 |
| `HNoDrainDeadline` | draining server waits forever for a hung turn | `HandoverEnds` | 83 |
| `HUntestedTarget` | untested build accepted by `handover` | `CurrentTested` | 17 |
| `HServeBroken` | broken build allowed to reach `SERVING` | `NeverServesBroken` | 26 |
| `HResumeIgnoresState` | `resume` ignores `handover/state` after a cut | `AlwaysRecoversToServing` | 108 |

## The code bound to the model

`HandoverTrace.tla` checks traces of `src/server/handover.cpp` emitted by `tests/server/handover_walk.cpp` over real
`Builds` folders and real `PosixFs` files and `flock`s. Accepted (`TraceUnfinished` violated at the final record): seeds
`1`, `2`, `3` at 600 steps (`tlc_handover_trace_seed_*`). Rejected (`pass`, no behaviour reaches the end): all 8 C++
faults (`tlc_handover_trace_*_rejected`):

| fault | seed | states matched | where the trace leaves the model |
|---|---|---|---|
| `no-lock` | 1 | 3 | first `lock` step: kernel `flock` probe sees `none` |
| `allow-untested` | 1 | 11 | `inspect` accepts untested build 3 |
| `no-sync` | 1 | 28 | `wswitch` skips `sswitch` |
| `in-memory-only` | 1 | 28 | `wswitch` writes no `handover/state` |
| `stop-cuts` | 1 | 36 | `drain` stops non-legacy server instead of draining |
| `no-rollback` | 1 | 73 | failed start of broken build skips `wrollback` |
| `serve-broken` | 1 | 73 | broken build answers `SERVING` |
| `overwrite-prev` | 2 | 84 | `inspect` on `to` ignores unfinished `handover/state` |
