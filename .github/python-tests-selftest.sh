#!/usr/bin/env bash
# python-tests.py against its planted cases (tests/ci/python-skips): each tier of a passing directory passes; a skipped
# test, an expected failure, a module that does not import, an empty tier and a list row naming no test each fail.
# Exits 1 if any case comes out otherwise. Run from the repository's root.
set -uo pipefail
cases=tests/ci/python-skips
out=$(mktemp "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/unittest-skips.XXXXXX")
trap 'rm -f "$out"' EXIT
bad=0
expect() {
  want=$1; shift
  if python3 .github/python-tests.py "$@" > "$out" 2>&1; then got=pass; else got=fail; fi
  if [ "$got" = "$want" ]; then
    echo "python-tests.py $*: $got, as planted"
  else
    echo "::error title=python-tests.py self-test::python-tests.py $* came out $got, not $want"
    cat "$out"
    bad=1
  fi
}
expect pass --tier pr --start "$cases/passes" --list "$cases/native.txt"
expect pass --tier native --start "$cases/passes" --list "$cases/native.txt"
expect fail --tier pr --start "$cases/skips" --list "$cases/native.txt"
expect fail --tier pr --start "$cases/xfail" --list "$cases/native.txt"
expect fail --tier pr --start "$cases/broken" --list "$cases/native.txt"
expect fail --tier native --start "$cases/skips" --list "$cases/native.txt"
expect fail --tier pr --start "$cases/passes" --list "$cases/unknown.txt"
exit "$bad"
