#!/usr/bin/env bash
# cmake/tiers.cmake against its planted cases (tests/ci/tiers): the good table puts each test in exactly one tier, as
# planted, and a table naming a missing test, a tier that is neither, two rows for one test, or a short row each fails
# the configure. tier_pr_build makes what the tier_pr tests need and nothing the nightly's alone need, and a tier_pr
# test whose program is a build-tree file no target makes fails the configure. Exits 1 if any case comes out
# otherwise. Run from the repository's root.
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
  if cmake --build "$work/good" --target tier_pr_build > "$work/out" 2>&1 && [ -f "$work/good/pr_tool.made" ] &&
      [ ! -e "$work/good/nightly_tool.made" ]; then
    echo "tiers.cmake, tier_pr_build: made the tier_pr test's pr_tool and not the nightly's nightly_tool, as planted"
  else
    echo "::error title=tiers.cmake self-test::tier_pr_build did not make pr_tool alone"
    cat "$work/out"
    bad=1
  fi
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
if cmake -S "$here/tests/ci/tiers" -B "$work/unbuilt" "-DNEXUS_TIERS_TABLE=$here/tests/ci/tiers/tables/good.txt" \
    -DPLANTED_UNBUILT=ON > "$work/out" 2>&1; then
  echo "::error title=tiers.cmake self-test::a tier_pr test running a file no target makes configured; it should fail"
  bad=1
else
  echo "tiers.cmake, a tier_pr test running a file no target makes: the configure fails, as planted ($(grep -m1 -o 'unbuilt_unit is a tier_pr test.*' "$work/out" | head -1))"
fi
exit "$bad"
