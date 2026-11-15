# models/placement: installed builds

`Placement.tla` is the model of placement (layer 2 of replacing a running server with a new build; `docs/builds.md`
says what the code does, `src/server/builds.cpp` is the code). It is checked on small instances with every invariant
and a liveness property, eight reversed witnesses show that each invariant can fail, and the code's own walks on real
files are checked to be behaviours of it.

## What is modelled

The builds directory: build folders named by their content hash (`B(t)`: tree t's name), pair folders (`P(c, p)`: the
links current and previous), the pointer `active` (the pair in force), and the leftovers of interrupted runs (`T`
`.tmp-*`, `Q` `.pair-*`, `N` `.active-*`, `G` `.gone-*`). A tree is a number: tree 1 is program 1 with driver 1, tree 2
program 2 with driver 2, tree 3 program 1 with driver 2 (tree 1 with its driver changed after its tests). A build's
three parts (`bin/`, `drivers/`, `formats`) each record the tree whose bytes they hold, so a half-copied or mixed folder
is visible.

Variables:

| variable | what it is |
|---|---|
| `pc`, `verb`, `tgt`, `ev`, `wf`, `np`, `frm`, `ph`, `cands` | each actor's next step and its verb's locals (the tree, its evidence, the folder it works on, the next part, the current it read, the candidates of a prune) |
| `lock` | the builds directory's lock: its holder or none |
| `pex` | a prune's exclusive lock on one build folder |
| `fl`, `fd` | each folder as it is now, and as it would survive a crash (its contents as last synced) |
| `dir`, `dirD`, `act`, `actD` | the directory's names and the pointer, now and as last synced |
| `pend` | the directory's changes since its last sync, in order (a rename is one change) |
| `sealed` | ghost: each folder's content when it was renamed to its hash name |
| `srv` | each server: down, resolved (it found its build's path) or up (holding its folder's lock shared) |
| `nextF`, `ops`, `crashes`, `deaths`, `serves` | bounds |

Actions, one per step of the code (its `Options::hook` phase names are the `pc` values):

- every mutating verb: `Start`, `LockTry` (one try; held by another, nothing changes), `LockGiveUp` (exit 4),
  `Settle` (fsync the directory), `CleanOne` (one leftover removed), `Unlock`;
- place: `MkTmp`, `Write` (one part), `Manifest` (the evidence checked against the tree's hash), `Sync`, `Verify`,
  `Rename`, `DirSync` (syncdir), `Drop`;
- current and back: `Check`, `PMk`, `PSync`, `PRename`, `DirSync` (pdsync), `ANew`, `ARename` (the one switch),
  `ASync`;
- prune: `PList`, `PTry` (the folder's lock tried without waiting), `PGone`, `PGSync`, `PRemove`;
- a server: `SResolve` (its path resolved to a build's name), `SHold` (its shared lock taken and the folder checked to
  be the one still under that name), `SStop`;
- outside events: `Die` (an actor killed: the disk as it is, its locks released), `Crash` (every process ends; each
  folder as now or as last synced; any ordered subset of the directory's unsynced changes survives).

Invariants: `TypeOK`; `CurrentValid` (current names a whole, tested build, at every instant, crash or not);
`PreviousValid`; `PointerResolves` (the pointer's pair is there and links the builds its name says);
`PrevIsLastCurrent` (previous is the build that was current before: the last good build is never lost);
`NamedWhole` (a folder under a hash name is whole); `SealedUnchanged` (a named build is never modified);
`InUseKept` (a build a running server holds is never removed or altered); `OneSwitcher`; `LockHeld` (every step past
the lock is its holder's).

Liveness: `VerbsEnd`, every verb begun ends (done, refused or busy), so an interrupted run never blocks a later one.
Fairness, in plain words: an actor that has begun a verb takes its next step; nothing else is assumed (no verb is
forced to start, and no death, crash or server start).

## Instances

| config | what | when | result |
|---|---|---|---|
| `Placement.cfg` | two actors, one verb each, from build 1 current alone; trees 1 to 3, tested 1 and 2; a death, a crash and a server start | per commit | pass: 306,768 distinct states, depth 30, 50 s on 4 workers |
| `PlacementSeq.cfg` | one actor, two verbs in a row | per commit | pass: 73,043 distinct states, depth 30, 9 s on 4 workers |
| `PlacementSeqThree.cfg` | one actor, three verbs in a row | nightly | pass: 883,490 distinct states, depth 43, 2 min 15 s on 2 workers |
| `PlacementLarge.cfg` | two actors, two verbs each | nightly | PLACEMENT_LARGE |

## Reversed witnesses

Each config in `witnesses/` sets `Bug` to one break of the code; TLC must report the invariant named.

| witness | the break | invariant | counterexample (read by hand against the code) |
|---|---|---|---|
| `PInPlace` | the build is written into the current build's folder | `CurrentValid` | 47 states, depth 7: the first part written over current's folder; current is no longer whole |
| `PNoSync` | the folder and the pair are not synced before their renames | `NamedWhole` | 183 states, depth 12: a crash keeps the rename and loses the contents |
| `PReuseTmp` | a dead run's `.tmp-*` folder is adopted and finished | `NamedWhole` | 2,170 states, depth 18: a half-placed folder renamed to a hash name it does not hold |
| `PTestedProgramOnly` | evidence that covers the program but not the driver is accepted | `CurrentValid` | 351 states, depth 27: tree 3 placed with tree 1's evidence and made current, untested |
| `PStaleMarker` | any evidence is accepted | `CurrentValid` | 961 states, depth 27: an untested tree marked tested and made current |
| `PTwoRenames` | the switch is two pointer renames (previous, then current) | `PrevIsLastCurrent` | 209 states, depth 13: between them the pair is (current, current), and the last good build is gone from it |
| `PNoLock` | no builds lock | `PrevIsLastCurrent` | 141,855 states, depth 22: two switches both read current 1 and both rename; one previous is lost |
| `PPruneInUse` | prune removes a build without its folder's lock | `InUseKept` | 636 states, depth 14: a server holds the build between the try and the rename aside |

## The code bound to the model

`PlacementTrace.tla` checks one trace of the real code. `tests/server/builds_walk.cpp` (functional: real files under
its own directory, threads) runs two actors and a server on a scratch builds directory, each parked at every
`Options::hook` step and at the lock's wait; a seeded random walk picks who steps next, kills an actor (an exception
thrown from the hook, so its descriptors close as on kill -9) or crashes the machine (every actor killed, the server
stopped). After each step it records what the disk and the system show: each actor's next step, who holds the lock (a
flock probe), current and previous through `builds/active`, the trees named, whole and tested, the number of
leftovers, and the server's state. It writes `PlacementTraceData.tla`.

The trace module takes, at each record, the action its op names and requires the state the code recorded. The model
chooses where the record does not say (which of a crash's unsynced changes survived, which candidate a prune takes
first), so a behaviour that chose otherwise ends early: the trace is accepted when some behaviour reaches its last
record, which TLC reports as `TraceUnfinished` violated, and rejected when none does (TLC ends with no error). Every
invariant is checked on every state of the way.

Accepted: seeds 1, 2, 3 at 800 steps (`tlc_placement_trace_seed_*`). Rejected: one walk per reversed fault in the code
(`builds_walk --fault`), each at a seed whose walk reaches the fault (`tlc_placement_trace_*_rejected`):

| fault | seed | records matched | where the trace leaves the model |
|---|---|---|---|
| `no-lock` | 1 | 2 | the first lock step: the system shows no lock held |
| `no-sync` | 1 | 22 | the manifest goes straight to verify |
| `tested-program-only` | 1 | 30 | the rename: tree 3 placed under evidence for tree 1 |
| `reuse-tmp` | 1 | 48 | settle goes to mktmp with a leftover in place |
| `prune-in-use` | 6 | 595 | prune's try goes on with the server up on the build |
| `two-renames` | 1 | 410 | the pointer's rename gives previous = current |

## Running

`ctest -L model -R tlc_placement` (per commit) and `ctest -L model_nightly -R tlc_placement` (nightly), through
`tests/store/tlc.cmake`; the walks need `builds_walk` built (`cmake --build build --target model_checks`).
