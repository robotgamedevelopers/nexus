#!/usr/bin/env bash
# ctest for CI, on Linux, macOS and WSL2 (Windows: ctest.ps1): ctest with the given arguments, where a selection that
# runs no test fails (--no-tests=error) and so does a test that did not run because it was skipped (its
# SKIP_RETURN_CODE) or disabled (DISABLED): no test is ever skipped. ctest lists those under "The following tests did
# not run:" and still exits 0, so this reads its output for that line. A test whose program is missing ("Not Run") is
# already a failure. tests/ci/ctest-skips plants each case (ctest-selftest.sh).
set -uo pipefail
log=$(mktemp "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/ctest.XXXXXX")
trap 'rm -f "$log"' EXIT
ctest "$@" --no-tests=error 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
if grep -q '^The following tests did not run:' "$log"; then
  echo "::error title=A test did not run::a test was skipped or disabled, and no test is ever skipped: make it run, or move it to the tier that can run it"
  sed -n '/^The following tests did not run:/,/^$/p' "$log"
  exit 1
fi
exit "$rc"
