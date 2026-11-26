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

## Who runs what

Per pull request (`ci.yml`), with `-L '^tier_pr$'`:

- `tests-macos`: macOS Debug.
- `tests-linux`: Linux.
- `windows`: Windows.
- `tailscale.yml`: the shim's unit tests.
- `server-policy`: the Python tests.
- `claude-driver`: the Claude driver's own unit tests.
- `watch`: the Apple Watch core's unit tests (the still-frames test among them), and the watchOS app compiled for the simulator and a watch (docs/watch.md).

Nightly (`nightly.yml`), with `-L '^tier_nightly$'`:

- `macos-debug`, `linux-nightly` and `windows-nightly`: the same configurations. They leave out the model checks, the Claude driver's conformance and the tests that time the real clock, which have their own jobs:
  - the `model*` jobs;
  - `claude-driver-macos` and `claude-driver-linux`;
  - `frame-budget`, `macos-release` and `server-lock`;
  - `watch-speech`: the watch's speech test, on hosted macOS.
- The nightly also runs configurations a pull request does not build: the Release build with LTO, the sanitizers, WSL2, the token walls, and the Tailscale shim's functional tests and Windows build.
- `python-native`: the Python tests that need a real Mac (`.github/python-native-tests.txt`), on the fleet Mac. `.github/python-tests.py` holds them to the same rule as ctest: a skip fails the run.

Every ctest call goes through `.github/ctest.sh` (`ctest.ps1` on Windows). It fails a skipped or disabled test and a selection of no tests.
