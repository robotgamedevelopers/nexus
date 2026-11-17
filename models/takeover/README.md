# models/takeover: end-to-end composition across all five takeover layers

`Takeover.tla` and `TakeoverTrace.tla` (`docs/roadmap.sexp`, `takeover-binding`, layer 6 of 6; `docs/builds.md` gives
the user-facing design) bind all five takeover layers operating together on one server home across old-layout migration,
build placement, handover, supervision, open-turn draining, post-start crashes, store-format upgrades, compatible and
refused rollbacks, and mid-flight process cuts:

1. **Layer 1 (`models/store/StoreLock.tla`, `src/store/store.cpp`)**: exclusive OS lock while serving or draining; shared
   lock for disk-format inspection (`Store::inspect_readers`).
2. **Layer 2 (`models/placement/Placement.tla`, `src/server/builds.cpp`)**: immutable placed builds verified by manifest
   hash and test evidence; `(current, previous)` switched in one rename, with disk-format rollback checks via
   `Builds::rollbackTo`.
3. **Layer 3 (`models/handover/Handover.tla`, `src/server/handover.cpp`)**: exclusive `flock` on `<dir>/handover`,
   durable stage records (`handover/state`), open-turn drain before replacement, startup verification (`SERVING`), and
   cut-anywhere `resume`.
4. **Layer 4 (`models/supervision/Supervision.tla`, `src/server/supervision.cpp`)**: exclusive `flock` on
   `<dir>/supervise`, durable crash counts and shunned builds (`supervise/state`), seamless restart across `drain`
   (`exit 75`), honored `stop` (`exit 0`), and post-start rollback under `<dir>/handover` — shunning `previous` without
   spinning and halting cleanly (`exit 78`) when a breaking store format blocks rollback.
5. **Layer 5 (`models/migration/Migration.tla`, `src/server/migration.cpp`, `src/store/store.cpp`)**: one-time
   `~/.nexus` -> `~/.nexus/server` move refusing live old servers on `~/.nexus/server.sock` before supervision or
   handover touches `<dir>`, and rollback-safe store replay (`3/3` <- `5/3` vs `5/5`).

## What is modelled

Variables:

| variable | what it is |
|---|---|
| `oldLayout`, `migrated` | `~/.nexus` old-layout state (`"none"`, `"cold"`, `"live"`) and whether old-layout migration completed |
| `placed`, `tested` | builds placed in `<dir>/builds` and builds carrying passing test evidence |
| `cur`, `prev` | active `(current, previous)` build pair in `<dir>/builds/active` (`1`: `3/3`, `2`: `5/3` compat, `3`: `5/5` breaking) |
| `hlock`, `hstate` | holder of `<dir>/handover`'s OS lock (`"none"`, `"ho"`, `"sup"`) and durable `handover/state` (`[stage, target, previous]`) |
| `supLock`, `supPc` | whether `supervise` holds `<dir>/supervise`'s OS lock and its current phase (`"idle"`, `"check"`, `"watch"`, `"reap"`, `"rollback"`) |
| `supDisk`, `explicitPick` | durable supervision state (`[build, crashes, shunned, halted, stopped]`) and explicit handover marker (`supervise/picked`) |
| `srv` | running server process (`[state \in {"down","serving","draining","exited"}, b, turn, code]`) |
| `maxReader`, `sessions`, `wroteBreaking` | highest `min_reader` byte in `<dir>/store` on disk (`0`, `3`, `5`), committed session count, and ghost breaking-write flag |
| `handovers`, `cuts`, `supDeaths`, `turns`, `stops` | bounds |

Actions:

- **Layer 5 (`migration`)**: `OldServerStop`, `Migrate` (`migrateOldLayout` refusing `"live"` and moving `"cold"` once);
- **Layer 2 (`builds`)**: `PlaceBuild(b, isTested)`, `SetInitialCurrent(b)`;
- **Layer 3 (`handover`)**: `HoBegin(t)`, `HoResumeAcquire`, `HoDrain`, `HoStart`, `HoRollback` (checking
  `RollbackFormatOk` via `Builds::rollbackTo`), `HoCut`;
- **Layer 1 & server runtime (`store`, `server`)**: `TurnOpen`, `TurnComplete`, `SrvWriteFormat` (`5/3` keeps
  `maxReader = 3`; `5/5` raises `maxReader = 5`), `SrvLateCrash`, `UserStop` (`nexus-server stop`);
- **Layer 4 (`supervision`)**: `SupStart` (runs `migrateOldLayout` first and exits cleanly without blaming `cur` if an
  old server is `"live"`), `SupCheck`, `SupWatch`, `SupReap`, `SupRollback` (checks `RollbackFormatOk` under `hlock` and
  shuns `prev` when refused so supervision never spins), `SupDie`.

Properties:

| property | kind | what it says |
|---|---|---|
| `TypeOK` | invariant | every state variable stays in its domain |
| `OldMigratedBeforeServe` | invariant | no server ever serves or drains while unmigrated state remains in `~/.nexus` |
| `CurrentAlwaysTested` | invariant | `cur` and any serving/draining build carry passing test evidence |
| `HandoverLockSerialized` | invariant | supervisor rollback always holds `<dir>/handover`'s OS lock with `handover/state` clear |
| `NeverIncompatibleCurrent` | invariant | `maxReader <= Writes(cur)` and `maxReader <= Writes(srv.b)` at every reachable state |
| `SessionsNeverLost` | invariant | sessions migrated from `~/.nexus` or committed across turns never vanish across upgrades or rollbacks |
| `LiveOldUntouched` | action | while an old server answers on `~/.nexus/server.sock`, no step migrates, switches or starts a new server |
| `OpenTurnPreserved` | action | an open turn on a non-crashing build is never cut by handover or supervision |
| `StopHonored` | action | intentional `stop` (`exit 0`) marks supervision stopped and stays stopped unless an explicit handover clears it |
| `DrainNeverStops` | action | reaping a `drain` exit (`75`) never marks supervision stopped |
| `SettlesOrHalts` | temporal | from any cut, crash, old-layout state or broken upgrade, the system settles cleanly without spinning |
| `WorkingFallbackServes` | temporal | whenever no breaking format-5 write occurs, no user stop occurs, and at least one slot of `(cur, prev)` is healthy, recovery reaches and keeps a healthy serving build |

## Instances

| config | what | when | result |
|---|---|---|---|
| `Takeover.cfg` | live old layout -> stop -> migrate -> builds `{1, 2, 3}` (`StartupBroken = {3}`, `LateBroken = {2}`, `Format5Writers = {2}`), handover, cut, supervisor death, open turn, stop (`Live`) | per commit | pass: 11,876 distinct states, ~2 s on 16 workers |
| `TakeoverFormatRollback.cfg` | builds `1 (3/3)`, `2 (5/3)`, `3 (5/5, LateBroken)` with `Format5Writers = {2, 3}` across two handovers, cut, supervisor death, and format writes before/after crash (`Live`) | per commit | pass: 7,658 distinct states, ~1 s on 16 workers |
| `TakeoverLarge.cfg` | live old layout + builds `1..3` (`LateBroken = {3}`, `Format5Writers = {2, 3}`), two handovers, two cuts, supervisor death, two turns, stop (`Live`) | nightly | pass: 94,129 distinct states, ~2 s on 16 workers |

## Reversed witnesses

Each config in `witnesses/` enables one break; TLC must reject it with the property named:

| witness | the break | property violated |
|---|---|---|
| `WTakeoverIgnoreLiveOld` | migrating while an old server answers on `~/.nexus/server.sock` | `LiveOldUntouched` |
| `WTakeoverSkipMigration` | serving `<dir>` while unmigrated `~/.nexus` files remain | `OldMigratedBeforeServe` |
| `WTakeoverServeUntested` | switching or serving without passing test evidence | `CurrentAlwaysTested` |
| `WTakeoverNoHandoverLock` | supervisor rollback skips `<dir>/handover`'s OS lock | `HandoverLockSerialized` |
| `WTakeoverBlindRollback` | rollback skips inspecting `<dir>/store`'s disk reader floor | `NeverIncompatibleCurrent` |
| `WTakeoverDrainCutsTurn` | handover cuts an open turn instead of draining | `OpenTurnPreserved` |
| `WTakeoverStopRestarts` | supervision restarts after intentional `nexus-server stop` | `StopHonored` |
| `WTakeoverDrainStops` | supervision treats `drain` (`exit 75`) as `stop` | `DrainNeverStops` |
| `WTakeoverVersionOnlyRollback` | rollback uses version-only `formatsRefusal` instead of disk reader check, blocking rollback from a crashed `5/5` build that never wrote format 5 | `WorkingFallbackServes` |
| `WTakeoverSpinOnFormatRefusal` | supervisor rollback fails to shun `previous` when disk-format rollback is refused, spinning forever | `SettlesOrHalts` |
| `WTakeoverBlameBuildForLiveOld` | `supervise` counts startup failures against `current` while a live old server holds `~/.nexus/server.sock` | `WorkingFallbackServes` |

## The code bound to the model

`TakeoverTrace.tla` checks traces emitted by `tests/server/takeover_walk.cpp` driving the real C++ `migrateOldLayout`,
`Builds`, `Handover`, `SupervisorLoop`, and `Store` engines together on real POSIX files and `flock` locks
(`takeover_walk <seed> <steps> [--fault <name>] <out.tla>`):

- Accepted (`tlc_takeover_trace_seed_1..4`): `TraceUnfinished` violated at the final record after full multi-layer
  lifecycles (live old layout -> migrate -> placement -> supervision -> handover -> open-turn drain -> mid-handover cut
  resume -> post-start crash rollback -> compatible `5/3` rollback -> breaking `5/5` write refusing `3/3` rollback and
  halting cleanly).
- Rejected (`tlc_takeover_trace_<fault>_rejected`): all 11 C++ cross-layer faults (`ignore-live-old`, `skip-migration`,
  `serve-untested`, `no-handover-lock`, `blind-rollback`, `drain-cuts-turn`, `stop-restarts`, `drain-stops`,
  `version-only-rollback`, `spin-on-format-refusal`, `blame-build-for-live-old`).

## Running

`ctest -L model -R '^tlc_takeover'` (per commit, split across `takeover-spec` and `takeover-rest` in CI) and
`ctest -L model_nightly -R '^tlc_takeover'` (nightly).
