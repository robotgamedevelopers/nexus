# CI tiers

Every test has exactly one tier, and no test is ever skipped.

| Tier | Runs | Holds | Limit |
|---|---|---|---|
| `tier_pr` | every pull request and push to main (`.github/workflows/ci.yml`, GitHub-hosted) | the fast unit tests, and one end-to-end happy path per feature | each job 2 minutes |
| `tier_nightly` | the nightly (`.github/workflows/nightly.yml`, from main) | every other test | none |

## How a test gets its tier

`cmake/tiers.cmake` adds the label `tier_pr` or `tier_nightly` to every test at the end of the configure:

- A test labelled `unit` is `tier_pr`.
- Any other test is `tier_nightly`.
- A row in `tests/tiers.txt` overrides the default for one test.

A row is `<test> <pr|nightly> <condition>`. The condition is the CMake options (joined with `+`) that are all on wherever the test is registered, or `ALWAYS`. The configure fails on any of these:

- a row whose condition holds but whose test does not exist;
- a malformed row;
- a second row for the same test;
- a test that already carries a tier label.

So a test the table names cannot quietly drop out of its tier. `.github/tiers-selftest.sh` checks all of this against planted cases (`tests/ci/tiers`) on every pull request.

A unit test stays per pull request only while it is fast. One that takes 1 s or more on a hosted runner moves to the nightly, by a row in the table. It is never skipped.

## What a pull request builds

The per-pull-request build jobs (`build-macos`, `build-linux`, `windows`) build `tier_pr_build` alone: the targets the `tier_pr` tests run, and nothing else. A change that relinks every program (the policy runtime's archive after a `policy/rules` edit, say) then relinks only those. The nightly and cache-warm build everything.

`cmake/test_targets.cmake` wraps `add_test` and records each test's command line; at the end of the configure `cmake/tiers.cmake` makes `tier_pr_build` depend on, for each `tier_pr` test, the target its command names and every target an argument names with `$<TARGET_FILE:...>` (or `_FILE_DIR`, `_LINKER_FILE`, `_BUNDLE_DIR`). A program a test starts by a path of its own goes in the test's `NEXUS_NEEDS` property. The configure fails for a `tier_pr` test registered before the wrapper, or one whose command is a build-tree file that no target makes. A program the list still misses fails its test, never skips it (`.github/ctest.sh`). `.github/tiers-selftest.sh` checks both against planted cases.

## Who runs what

Per pull request (`ci.yml`), with `-L '^tier_pr$'`:

- `tests-macos`: macOS Debug.
- `tests-linux`: Linux.
- `windows`: Windows.
- `tailscale.yml`: the shim's unit tests.
- `server-policy`: the Python tests.
- `claude-driver`: the Claude driver's own unit tests.
- `watch`: the Apple Watch core's unit tests (the still-frames test among them) from the watch's own build (`cmake -S watch`, where every test is a fast unit test, so it selects `-L unit`), and the watchOS app compiled for the simulator and a watch (docs/watch.md). nexus's own build leaves the watch out of its default targets; the nightly builds its walks and speech tools by name.

Nightly (`nightly.yml`), with `-L '^tier_nightly$'`:

- `macos-debug`, `linux-nightly` and `windows-nightly`: the same configurations. They leave out the model checks, the Claude driver's conformance and the tests that time the real clock, which have their own jobs:
  - the `model*` jobs;
  - `claude-driver-macos` and `claude-driver-linux`;
  - `frame-budget`, `macos-release` and `server-lock`;
  - `watch-speech`: the watch's speech test, on hosted macOS.
- The nightly also runs configurations a pull request does not build: the Release build with LTO, the sanitizers, WSL2, the token walls, and the Tailscale shim's functional tests and Windows build.
- `python-native`: the Python tests that need a real Mac (`.github/python-native-tests.txt`), on the fleet Mac. `.github/python-tests.py` holds them to the same rule as ctest: a skip fails the run.

Every ctest call goes through `.github/ctest.sh` (`ctest.ps1` on Windows). It fails a skipped or disabled test and a selection of no tests.
