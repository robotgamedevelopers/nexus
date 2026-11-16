# Installed builds

How nexus-server keeps the builds it runs from: each one immutable, complete, and switched to in one atomic rename.
This is placement, layer 2 of replacing a running server with a new build (docs/roadmap.sexp, `takeover-placement`):
it places builds and switches between them, and never starts, stops or signals a server, writes a service file or
touches the store. The code is `src/server/builds.h` (the verbs) and `src/server/build_manifest.h` (the text and the
decisions); the model is `models/placement/Placement.tla`.

## The layout

Under the server's directory (`~/.nexus/server`, or `--dir`):

```
builds/
  <hash>/                 a build, named by the hash of everything in it but its manifest
    bin/nexus-server      the server program (executable); bwrap and socat beside it on Linux
    drivers/              the driver tree (drivers/claude/...)
    nexus.app/            the window application, where it is placed (macOS)
    formats               the formats this build writes and reads (below)
    manifest              every file, directory and link in the folder, with its hash
  pair-<c>-<p>/           a pair: current -> ../<c>, previous -> ../<p> (pair-<c>-none: no previous)
  active -> pair-<c>-<p>  the pointer: the one pair in force
  .tmp-*  .pair-*  .active-*  .gone-*
                          what an interrupted run left: removed by the next run that takes the lock
current -> builds/active/current
                          the stable path
```

Once a folder has its hash name it is read-only (directories 0555, files 0555 or 0444) and nothing writes it again;
a build is removed whole or not at all.

The stable path, `<server dir>/current`, resolves through the pointer to the current build, so every path that names
the running build goes through it and moves with one rename: the program on the PATH
(`<server dir>/current/bin/nexus-server`), a driver in the drivers file (`<server dir>/current/drivers/claude/...`),
and the application link (`<server dir>/current/nexus.app`). `current` makes the stable link the first time; making
the PATH link, the drivers file and the application link is the install's (a later layer).

A running server finds its build from its own resolved path (`builds/<hash>/bin/nexus-server`), holds that folder's
lock shared for its life, and answers `status` with it: `nexus-server status` prints `SERVING` and `build <hash>` (or
`build none`, a program outside the placed builds, such as a build tree).

## The manifest and the hash

```
nexus-build 1
tree 3f2a...                         the folder's name: the hash below
format store-record 3 3              the formats file, one line each
format store-snapshot 4 4
format config 2 2
format link 4 1
format driver-protocol 1 1
tested 3f2a... pass ctest: 412 tests, 0 failed
dir	bin
file	x	1834512	9c1e...	bin/nexus-server
dir	drivers
link	claude/main.js	drivers/main.js
file	-	2210	77ab...	formats
end
```

Each entry is a tab-separated line: `dir <path>`, `file <x|-> <size> <sha256> <path>` (x: executable), `link <target>
<path>` (a relative link inside the build). The tree hash is the SHA-256 of every entry line in path order: the
program, the whole driver tree and the formats file are in it, so a driver changed is another build. A manifest
without `end`, or whose tree is not the hash of the entries it lists, does not read, and the build is damaged.

## What binds tested to the content

A build is tested only by evidence that names its exact hash. Whatever ran the tests writes, after they passed:

```
nexus-test-evidence 1
tree <the hash `nexus-server builds hash <tree>` printed for the tree that was tested>
result pass
run <one line: what ran, and its counts>
```

`place --evidence` hashes the tree as it copies it and refuses evidence for any other hash: a file changed since the
tests (the driver tree among them) is other bytes, and evidence for another tree is not this one's. The `tested` line
goes into the manifest, which names the same hash, inside the read-only folder named by that hash; `current` reads the
whole folder back against the manifest before it switches. So the mark cannot cover the program but not the driver,
cannot be stale (it lives in the folder whose bytes it names), and cannot be added later to a build placed untested:
that build is removed and placed again.

## The formats rule

The formats file lists `format <name> <writes> <reader>`: the version the build writes, and the oldest reader that
can read it (`nexus-server builds formats` prints this program's). `current` and `back` refuse, in one line, a build
that cannot read what the current one writes: for each of `store-record`, `store-snapshot` (store/store.h) and
`config` (accounts.json and friends.json), the current build's `reader` must be at most the target's `writes`. The
link's state format and the driver protocol are agreed between their two ends at each connection, and are listed but
not checked. Whether the store holds anything the target cannot read is the store's own check, at layer 5; this rule
goes by the versions alone.

## The lock

Every verb that changes anything takes `flock(LOCK_EX)` on the builds directory's own descriptor, waiting up to 60 s
(then exit 4), and holds it until it ends; the OS releases it however the process ends. Where it is is checked as the
store's lock is (store/fs.h): a network or FUSE filesystem is refused, and so is one where a second exclusive lock is
granted. Under the lock the directory is synced first (so that a rename an interrupted run made is durable before it
is judged), and then every leftover is removed: only the lock's holder makes `.tmp-`, `.pair-`, `.active-` and
`.gone-` names and pairs, so whoever made one has ended. `list` and `verify` take the lock shared.

## Placing

`nexus-server builds place <tree> [--evidence <file>]`, step by step (the model's names), and what a crash between
two steps leaves:

| step | what it does | a crash after it leaves |
|---|---|---|
| lock, settle, clean | the lock; fsync the directory; remove leftovers | the builds as they were |
| mktmp | `mkdirat .tmp-<pid>-<n>` | a leftover, removed by the next run |
| write (per part) | each file: `openat(O_CREAT\|O_EXCL\|O_NOFOLLOW)`, every byte hashed and written, `fchmod`, `F_FULLFSYNC`/`fsync`, `close` checked; links by `symlinkat`, directories by `mkdirat` | a leftover |
| manifest | the evidence checked against the tree's hash (refused: the folder is dropped); the manifest written and synced | a leftover |
| sync | directories made 0555 from the leaves up, each synced, the folder last | a leftover |
| verify | every file read back against the manifest, nothing more | a leftover |
| rename | one `renameat(.tmp-…, <hash>)`; a build already under the name is kept and this copy dropped | the build under its name, whole (it was synced before), or the leftover |
| syncdir | fsync the directory | the build, durable |
| unlock | | |

Placing never changes a build already placed or which build is current. Every call's result is checked: a failed
write, sync, close, rename or link (a full disk among them) is said in one line, exit 1, and the folder is dropped.

## Switching

`nexus-server builds current <hash>` (and `back`, which makes previous current):

| step | what it does | a crash after it leaves |
|---|---|---|
| lock, settle, clean | as placing | the old pair in force |
| check | the target placed, read back whole, tested, and the formats rule kept; the stable link made if it is not there | the old pair |
| pmk, psync | `.pair-<pid>-<n>/` with the links `current -> ../<target>` and `previous -> ../<current>`, synced | the old pair; a leftover |
| prename, pdsync | renamed to `pair-<target>-<current>`; fsync the directory | the old pair; a pair not in force (a leftover) |
| anew | `symlinkat(pair-<target>-<current>, .active-<pid>-<n>)` | the old pair; a leftover |
| arename | THE switch: one `renameat(.active-…, active)` | the old pair or the new, never a mix |
| async | fsync the directory | the new pair, durable |
| unlock | | |

Current and previous change together, in that one rename: there is no moment when one has moved and the other has
not, and previous is always the build that was current before (`PrevIsLastCurrent`).

## Retention

`prune [--keep <n>]` removes every build but current, previous and the n (default 2) most recently placed of the
rest; `remove <hash>` removes one (never current or previous). Each, under the builds lock: the folder's own lock tried
exclusive without waiting (refused: a running server holds it shared, and the build is kept), the folder renamed to
`.gone-…`, the directory synced, the folder's lock released, the folder removed. A server starting from a build being
removed does not start (its shared lock is refused, or its folder is no longer under the name).

## The verbs

`nexus-server builds help [<verb>]` prints each verb's steps, outputs and exit statuses: `place`, `hash`, `formats`,
`list`, `verify`, `current`, `back`, `prune`, `remove`. Exit statuses: 0 done (or nothing to do, said so); 1 a
filesystem call failed, a build is damaged, or locks cannot be trusted where the directory is; 2 usage; 3 refused by a
rule (untested, the formats rule, not placed, in use, a tree a build cannot be, evidence for other bytes); 4 the lock
was held for the whole wait. Every failure is one plain line on standard error.

```
$ nexus-server builds hash ./stage
3f2a...
$ nexus-server builds place ./stage --evidence ./stage.evidence
placed 3f2a... tested
$ nexus-server builds current 3f2a...
current 3f2a...
previous 9be0...
$ nexus-server builds list
9be0... previous tested - formats:store-record=3/3,store-snapshot=4/4,config=2/2,link=4/1,driver-protocol=1/1
3f2a... current tested in-use formats:store-record=3/3,store-snapshot=4/4,config=2/2,link=4/1,driver-protocol=1/1
```

## The model and its tests

`models/placement/README.md` lists the model's variables, actions, invariants and liveness, its reversed witnesses,
and the trace walk that binds the code to it. The tests: `builds_unit_*` (unit: the text and the decisions),
`builds_functional_*` (functional: real files, real crashes), `tlc_placement_*` (the model, its witnesses and the
walks).

## Handover

Layer 3 (`docs/roadmap.sexp`, `takeover-handover`; `src/server/handover.h`, `src/server/drain.h`,
`models/handover/Handover.tla`) replaces the running server with a placed build under an OS lock, keeping durable
step-by-step state on disk so a handover cut anywhere resumes from what is on disk.

Under `<server dir>/`:

```
handover/
  gate                    writers' gate (store/fs.h)
  state                   active handover record (absent when no handover is in flight)
  .state-*                interrupted state write, removed by the next holder after syncing handover/
```

`handover/state` is written through a synced `.state-*` file, renamed over `state`, with `handover/` synced before each
stage acts:

```
nexus-handover 1
target 3f2a...
previous 9be0...
stage switching
end
```

`stage` is one of `switching`, `draining`, `starting`, or `rolling-back`; `previous` is `none` when no working build
was current before.

### Steps and crash recovery

Every `nexus-server handover <hash>` or `nexus-server handover [resume]` takes `flock(LOCK_EX)` on `handover/`'s own
descriptor (checked with `unlockable_filesystem` and `locks_kept`), syncs `handover/`, removes `.state-*` leftovers,
and inspects `handover/state` first:

1. **Unfinished `handover/state` present**: whatever command acquired the lock finishes resuming that interrupted
   handover first before starting any new target, so `previous` (the last working build) is never overwritten by a
   broken build that never reached `SERVING`.
2. **`switching`**: switches `builds/active` atomically to `(target, previous)` (`Builds::makeCurrent`).
3. **`draining`**: asks the running server on `server.sock` to `drain` (`nexus-server drain`). While draining, the
   server keeps answering `SERVING` (`build <hash> draining`) and serving open windows until no session has an open
   turn or starting driver (or its 10-minute drain deadline passes), then closes `server.sock`, ends its drivers, and
   exits `75` (`EX_TEMPFAIL`). A server from before `drain` is stopped with `stop`.
4. **`starting`**: starts `<server dir>/current/bin/nexus-server serve --dir <server dir>` and waits up to 30 s for
   `status` to answer `SERVING` with `build <target>`.
5. **`rolling-back`**: if `target` exits non-zero or does not answer `SERVING` in time, any hung started child is
   ended, `rolling-back` is synced to `handover/state`, `builds/active` is switched back to `previous`, `previous` is
   started until it answers `SERVING`, and `handover/state` is removed and synced.

Exit statuses of `nexus-server handover`: `0` ok (for `resume`, recovered to `SERVING`); `1` failed; `2` usage; `3`
refused (not placed, untested, incompatible formats, no current build); `4` lock busy; `5` target failed to serve and
rolled back to `previous`.

## Supervision

Layer 4 (`docs/roadmap.sexp`, `takeover-supervision`; `src/server/supervision.h`, `src/server/supervise_state.h`,
`models/supervision/Supervision.tla`) keeps the active placed build running across boots, drain handovers (`exit 75`),
intentional stops (`exit 0`), and post-start build failures (a build that reaches `SERVING` during handover and crashes
later).

Under `<server dir>/`:

```
supervise/
  gate                    writers' gate (store/fs.h)
  state                   durable supervision record (written under supervise/'s flock)
  stopped                 atomic intentional-stop marker (created by stop / Reap(0))
  picked                  atomic explicit-handover target marker (<hash>\n)
  .state-*                interrupted state/marker writes, removed by the next holder after syncing supervise/
```

`supervise/state` is written through a synced `.state-*` file, renamed over `state`, with `supervise/` synced:

```
nexus-supervise 1
build 3f2a...
crashes 0
halted 0
stopped 0
shun 7cd1...
end
```

### Lifecycle, post-start rollback, and shunning

`launchd` (macOS LaunchAgent) and `systemd` (Linux and WSL2 user unit) run:

```
<server dir>/current/bin/nexus-server supervise --dir <server dir>
```

At most one supervisor runs per `<server dir>` under `flock(LOCK_EX)` on `supervise/`'s own descriptor:

1. **Cold start and handover adoption**: checks `handover/`'s lock (waiting if an external `handover` is in flight, and
   resuming any interrupted `handover/state` left unlocked on disk), verifies `builds/active`'s `current` build has
   passing test evidence, and either adopts an already-serving `current` server or spawns
   `<server dir>/current/bin/nexus-server serve --dir <server dir>`.
2. **Drain (`exit 75`)**: when `serve` finishes draining open turns and exits `75`, the supervisor resets `crashes` to
   `0` and adopts or starts the active `current` build once `handover/` is unlocked.
3. **Intentional stop (`exit 0`) vs OS shutdown (`SIGTERM`)**: `nexus-server stop` creates the atomic marker
   `supervise/stopped` before `serve` exits `0`, so background `supervise/state` writes never clobber an intentional
   stop, the supervisor exits `0`, and any subsequent restart stays stopped until `nexus-server supervise --start` or
   `nexus-server handover <hash>` clears `supervise/stopped`. `SIGTERM` or `SIGINT` sent to `supervise` during OS
   shutdown stops `serve` cleanly and exits `0` without setting `supervise/stopped`, so the next boot starts `current`.
4. **Post-start failure rollback and shunning**: if `current` crashes (`exit != 0, 75`) `2` times
   (`kDefaultMaxCrashesBeforeRollback`) after starting and an unshunned `previous` build exists in `builds/active`, the
   supervisor acquires `handover/`'s exclusive lock, records the failed build in `shun`, switches `builds/active` back
   to `previous`, and starts `previous`. Persisting `shun` in `supervise/state` prevents a dual-broken pair from
   ping-ponging across supervisor restarts; an explicit `nexus-server handover <hash>` unshuns `<hash>`. Once a build
   serves stably for `60 s` (`kStableMs`), `crashes` resets to `0` while keeping `shun`.
5. **Bounded halt (`exit 78`)**: if no unshunned `previous` build exists, the supervisor backs off and halts after `5`
   consecutive crashes (`kDefaultMaxRestarts`), persisting `halted 1` and exiting `78` (`EX_CONFIG`).

### OS service units

`nexus-server supervise unit --kind systemd|launchd` prints the deterministic service definition without invoking
`systemctl` or `launchctl`:

- **Linux and WSL2 (`systemd` user unit, `~/.config/systemd/user/nexus-server.service`)**: `ExecStart` runs
  `<server dir>/current/bin/nexus-server supervise --dir <server dir>` with `KillMode=process`, `Restart=on-failure`,
  `RestartPreventExitStatus=3 78`, and `WantedBy=default.target`.
- **macOS (`launchd` LaunchAgent, `~/Library/LaunchAgents/com.robotgamedevelopers.nexus-server.plist`)**:
  `ProgramArguments` runs `[program, "supervise", "--launchd", "--dir", serverDir]` with `RunAtLoad=true`,
  `KeepAlive.SuccessfulExit=false`, and `ThrottleInterval=10` (`--launchd` persists `halted 1` and exits `0` when
  permanently halted or refused so `launchd` does not respawn a permanently halted supervisor every 10 s).
- Non-default `--dir` paths append a deterministic 12-hex SHA-256 suffix to the unit/label name so isolated instances
  never collide.

Exit statuses of `nexus-server supervise`: `0` stopped cleanly; `1` failed; `2` usage; `3` refused (no current build or
untested); `4` lock busy; `78` halted after exhausting rollback and restart bounds.
