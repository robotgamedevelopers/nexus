# models/supervision: service supervision, post-start rollback, and shunning

`Supervision.tla` models layer 4 of replacing a running server with a placed build (`docs/roadmap.sexp`,
`takeover-supervision`; `docs/builds.md` says what the code does, `src/server/supervision.cpp` and
`src/server/supervise_state.cpp` are the code). Layer 1 (`models/store/StoreLock.tla`) keeps at most one writer on the
store; layer 2 (`models/placement/Placement.tla`) places immutable builds and switches `(current, previous)` in one
rename; layer 3 (`models/handover/Handover.tla`) drains the old server and verifies a newly switched build reaches
`SERVING`. Layer 4 sits above them: `launchd` and `systemd` run `nexus-server supervise`, which keeps the active build
running across boots, restarts cleanly across `drain` handovers (`exit 75`), leaves intentional `stop` (`exit 0`)
stopped, and rolls `builds/active` back to `previous` when a build crashes after reaching `SERVING`—persisting shunned
builds and crash counts on disk so a dual-broken pair never ping-pongs across supervisor restarts.

## What is modelled

`<server dir>/supervise` (`supLock`, `disk`), `<server dir>/handover`'s exclusive lock (`hlock`), `builds/active`'s
`(cur, prev)` pair, and the supervised server process `child` (`[b, state, code]`).

Variables:

| variable | what it is |
|---|---|
| `pc`, `sbuild`, `scrashes`, `sshunned` | each supervisor actor's next step (matching `Options::hook` in C++) and its locals |
| `supLock` | holder of `supervise/`'s exclusive `flock`, or `None` |
| `hlock` | holder of `handover/`'s exclusive `flock` (a supervisor rolling back, `HandoverHolder`, or `None`) |
| `hTarget`, `explicitPick` | target of an in-flight or completed explicit `handover <hash>` (unshuns that build once) |
| `cur`, `prev` | `builds/active` current and previous build |
| `disk` | durable `<server dir>/supervise/state`: `[build, crashes, shunned, halted, stopped]` |
| `child` | supervised server process: `b` (`0` or `1..3`), `state` (`"down"`, `"serving"`, `"exited"`), `code` (`0`, `75`, `1`) |
| `stopped`, `halted` | service-level clean-stop (`nexus-server stop`) and exhausted-bounds halt (`exit 78`) flags |
| `rbHistory` | ghost set of builds rolled back from since the last explicit handover to them |
| `races`, `supDeaths`, `handovers`, `stops` | bounds |

Actions, one per step of `SupervisorLoop::Run` (`Options::hook`):

- lock and durable state load: `EnsureSup`, `RaceSupStart`, `SupLockTry`, `SupLockGiveUp` (exit 4), `SupLoad`
  (`supervise/` settled and `supervise/state` read; honors `disk.stopped` and `disk.halted` across restarts), `SupUnlock`;
- lifecycle and supervision loop: `SupCheck` (waits while external `handover` holds `hlock`, refuses untested `cur`,
  adopts a serving child, or dispatches to `spawn`, `reap`, `rb_lock`, or halt), `SupSpawn`, `SupWatch` (moves to `reap`
  when `child` exits, or drains a stale `child.b # cur` when `hlock = None`), `SupReap` (`0` -> `stopped`; `75` ->
  `check`; crash -> `rb_lock`, `backoff`, or halt `78`), `SupBackoff`, `ChildStable` (resets `crashes` past stability
  window while keeping `shunned`);
- post-start rollback under the handover lock: `SupRbLock` (takes `hlock`), `SupRollback` (adds failed `cur` to
  `shunned`, switches `builds/active` back to unshunned `prev`), `SupRbUnlock`;
- server and environment: `ChildLateCrash` (post-`SERVING` crash of a `LateBroken` build), `UserStop` (`nexus-server
  stop` writes `disk.stopped = TRUE` and exits `0`), `HandoverBegin` / `HandoverFinish` (external `handover <t>`),
  `SupDie` (`SIGKILL` of supervisor: OS locks released, `supervise/state` preserved on disk).

Properties:

| property | kind | what it says |
|---|---|---|
| `TypeOK` | invariant | all state variables stay in their domains |
| `OneSupervisor` | invariant | at most one supervisor actor is past `"lock"`, and that actor holds `supLock` |
| `HandoverSerialized` | invariant | every supervisor in `"rollback"` or `"rb_unlock"` holds `hlock` |
| `CurrentTested` | invariant | `cur` is always in `Tested` |
| `RestartsBounded` | invariant | consecutive crashes without halting never exceed `MaxRestarts` in memory or on disk |
| `NeverRollsBackToShunned` | action | `rollback` never switches `cur` to a build already in `rbHistory` |
| `StopStaysStopped` | action | reaping `ExitStop` (`0`) marks `stopped`, and while `disk.stopped` holds no child spawns |
| `DrainRestartsCurrent` | action | reaping `ExitDrain` (`75`) never marks the service `stopped` |
| `SupervisorSettles` | temporal | supervision always settles: serving a healthy tested build, `stopped`, or `halted` |
| `SettledHealthy` | temporal | when `Initial` is healthy and no `UserStop` occurs, handover to a `LateBroken` build rolls back after post-start crashes and settles serving `Initial` |

## Instances

| config | what | when | result |
|---|---|---|---|
| `Supervision.cfg` | two supervisors, builds `{1, 2}`, `LateBroken = {2}`, lock race, supervisor death, handover, stop | per commit | pass: 1,914 distinct states, 13 s on 4 workers |
| `SupervisionRollback.cfg` | `RollbackLive`: handover from healthy `1` to `LateBroken` `2` with supervisor death (`SettledHealthy`) | per commit | pass: 748 distinct states, 19 s on 4 workers |
| `SupervisionDualFail.cfg` | both `{1, 2}` in `LateBroken`, supervisor death + handover (`NeverRollsBackToShunned`, `SupervisorSettles`) | per commit | pass: 2,936 distinct states, 11 s on 4 workers |
| `SupervisionLarge.cfg` | builds `{1, 2, 3}`, `StartupBroken = {3}`, `LateBroken = {2}`, two handovers, two supervisor deaths, stop | nightly | pass: 17,856 distinct states, 23 s on 8 workers |

## Reversed witnesses

Each config in `witnesses/` enables one break; TLC must reject it with the property named:

| witness | the break | property | states |
|---|---|---|---|
| `SNoSupLock` | `supervise/`'s `flock` is skipped | `OneSupervisor` | 7 |
| `SNoHandoverLock` | `rollback` switches `builds/active` without `handover/`'s `flock` | `HandoverSerialized` | 46 |
| `SServeUntested` | untested build accepted as `current` | `CurrentTested` | 24 |
| `SStopRestarts` | `ExitStop` (`0`) restarts instead of staying stopped | `StopStaysStopped` | 9 |
| `SDrainStops` | `ExitDrain` (`75`) stops supervision instead of starting `current` | `DrainRestartsCurrent` | 43 |
| `SNoPostStartRollback` | post-start crashes keep restarting broken `current` instead of rolling back to `previous` | `SettledHealthy` | 58 |
| `SPingPongRollback` | `shunned` ignored on rollback so two broken builds ping-pong forever | `NeverRollsBackToShunned` | 190 |
| `SInMemoryState` | `shunned` kept in memory only, lost when supervisor restarts (`SupDie`) | `NeverRollsBackToShunned` | 418 |
| `SNoHaltBound` | crash loop past `MaxRestarts` never halts when no rollback build is eligible | `SupervisorSettles` | 32 |

## The code bound to the model

`SupervisionTrace.tla` checks traces of `src/server/supervision.cpp` emitted by `tests/server/supervision_walk.cpp` over
real `Builds` folders and real `PosixFs` files and `flock`s. Accepted (`TraceUnfinished` violated at the final record):
seeds `1`, `2`, `3` at 350 steps (`tlc_supervision_trace_seed_*`). Rejected (`pass`, no behaviour reaches the end): all
9 C++ faults (`tlc_supervision_trace_*_rejected`):

| fault | seed | states matched | where the trace leaves the model |
|---|---|---|---|
| `serve-untested` | 1 | 1 | `ho_begin(3)` accepts untested build 3 |
| `no-sup-lock` | 1 | 2 | `race_start` + `lock` advances past `"lock"` without `supLock` |
| `in-memory-state` | 1 | 5 | `check` persists no `supervise/state` on disk |
| `drain-stops` | 1 | 16 | `reap` of `ExitDrain` (`75`) marks `stopped` instead of restarting `current` |
| `ping-pong-rollback` | 2 | 22 | `rollback` switches `builds/active` without shunning failed build |
| `no-post-start-rollback` | 1 | 24 | 2nd post-start crash backs off instead of entering `rb_lock` |
| `no-handover-lock` | 1 | 25 | `rb_lock` enters `rollback` without `handover/`'s `flock` |
| `stop-restarts` | 1 | 31 | `reap` of `ExitStop` (`0`) returns to `check` instead of stopping |
| `no-halt-bound` | 2 | 44 | crash at `MaxRestarts` backs off instead of halting (`78`) |
