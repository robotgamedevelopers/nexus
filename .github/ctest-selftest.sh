#!/usr/bin/env bash
# ctest.sh against its planted cases (tests/ci/ctest-skips): a passing test passes; a skipped test, a disabled test and
# a selection of no tests each fail. Exits 1 if any case comes out otherwise.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/ctest-skips.XXXXXX")
trap 'rm -rf "$build"' EXIT
cmake -S "$here/tests/ci/ctest-skips" -B "$build" > /dev/null || exit 1
bad=0
expect() {
  want=$1; shift
  if bash "$here/.github/ctest.sh" --test-dir "$build" "$@" > "$build/out" 2>&1; then got=pass; else got=fail; fi
  if [ "$got" = "$want" ]; then
    echo "ctest.sh $*: $got, as planted"
  else
    echo "::error title=ctest.sh self-test::ctest.sh $* came out $got, not $want"
    cat "$build/out"
    bad=1
  fi
}
expect pass -L '^passes$'
expect fail -L '^skips$'
expect fail -L '^disabled$'
expect fail -L '^matches-nothing$'
exit "$bad"
