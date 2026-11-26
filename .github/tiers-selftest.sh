#!/usr/bin/env bash
# cmake/tiers.cmake against its planted cases (tests/ci/tiers): the good table puts each test in exactly one tier, as
# planted, and a table naming a missing test, a tier that is neither, two rows for one test, or a short row each fails
# the configure. Exits 1 if any case comes out otherwise. Run from the repository's root.
set -uo pipefail
here=$(pwd)
work=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tiers.XXXXXX")
trap 'rm -rf "$work"' EXIT
bad=0
names() { ctest --test-dir "$work/good" -N "$@" | sed -n 's/^ *Test *#[0-9]*: //p' | sort | tr '\n' ' '; }
if cmake -S "$here/tests/ci/tiers" -B "$work/good" "-DNEXUS_TIERS_TABLE=$here/tests/ci/tiers/tables/good.txt" > "$work/out" 2>&1; then
  pr=$(names -L '^tier_pr$')
  nightly=$(names -L '^tier_nightly$')
  all=$(names)
  both=$(names -L '^tier_pr$' -L '^tier_nightly$')
  for check in "tier_pr|$pr|fast_unit happy_path " "tier_nightly|$nightly|functional_one no_label slow_unit " \
               "every test|$all|fast_unit functional_one happy_path no_label slow_unit " "both tiers|$both|"; do
    IFS='|' read -r what got want <<< "$check"
    if [ "$got" = "$want" ]; then
      echo "tiers.cmake, the good table, $what: ${got:-none}, as planted"
    else
      echo "::error title=tiers.cmake self-test::the good table's $what came out '${got}', not '${want}'"
      bad=1
    fi
  done
else
  echo "::error title=tiers.cmake self-test::the good table failed the configure"
  cat "$work/out"
  bad=1
fi
for table in missing bad-tier twice short; do
  if cmake -S "$here/tests/ci/tiers" -B "$work/$table" "-DNEXUS_TIERS_TABLE=$here/tests/ci/tiers/tables/$table.txt" > "$work/out" 2>&1; then
    echo "::error title=tiers.cmake self-test::the $table table configured; it should fail"
    bad=1
  else
    echo "tiers.cmake, the $table table: the configure fails, as planted ($(grep -m1 -o 'tests/tiers.txt.*\|.*already has a tier.*' "$work/out" | head -1))"
  fi
done
exit "$bad"
