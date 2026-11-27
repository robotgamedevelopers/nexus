#!/usr/bin/env bash
# source-times.sh own's planted cases (bash .github/source-times.sh --selftest; ci.yml's runners job). No real time:
# every time is given, and the build is a stand-in that copies src.txt to out when src.txt is newer (as ninja and
# make decide), stamping out with the time it is given.
#
# The case of 2026-11-26 (cache-warm run 45861944639, mac-debug linked a libnexus_driver.a without #87's steering):
# one runner's checkout is shared by its build directories. mac-debug built protocol.cpp, then an iOS directory saw
# the next commit and later set that file's time back (the old apply, by its own record); mac-debug's next build found
# the file older than its object and kept the stale one. Here A is mac-debug and B the iOS directory: with `own`, A
# rebuilds; the same steps through the old apply and record (the reversed witness) leave A stale, as on the runner.
set -uo pipefail
script=${1:?usage: source-times-test.sh path/to/source-times.sh}
script=$(cd "$(dirname "$script")" && pwd)/$(basename "$script")
work=$(mktemp -d "$PWD/.source-times-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
bad=0
fail() { echo "::error title=source-times.sh self-test::$*"; bad=1; }

# A fresh checkout holding src.txt (and one file that never changes), and two build directories beside it.
setup() {
  rm -rf "$work/case"
  mkdir -p "$work/case/repo" "$work/case/A" "$work/case/B"
  git -C "$work/case/repo" init -q
  printf 'same\n' > "$work/case/repo/same.txt"
  checkout v1 202601010000.00
}
# The checkout moving to content $1, rewriting src.txt at time $2 (a checkout rewrites only what changed).
checkout() {
  printf '%s\n' "$1" > "$work/case/repo/src.txt"
  touch -t "$2" "$work/case/repo/src.txt"
  git -C "$work/case/repo" add -A
  git -C "$work/case/repo" -c user.name=t -c user.email=t@example.invalid commit -q -m "$1"
}
# A job's source times for build directory $1 at time $2.
own() { (cd "$work/case/repo" && bash "$script" own "$work/case/$1/.nexus-ci-times" "$2") > /dev/null || fail "own $1 $2 failed"; }
# The stand-in build of directory $1, finishing at time $2.
build() {
  local out="$work/case/$1/out"
  if [ ! -e "$out" ] || [ "$work/case/repo/src.txt" -nt "$out" ]; then
    cp "$work/case/repo/src.txt" "$out"
    touch -t "$2" "$out"
    echo built >> "$work/case/$1/builds"
  fi
}
holds() { cat "$work/case/$1/out"; }
builds() { if [ -f "$work/case/$1/builds" ]; then wc -l < "$work/case/$1/builds" | tr -d ' '; else echo 0; fi; }

# 1. The incident, with own: A rebuilds the content B saw first.
setup
own A 202601010100.00; build A 202601010101.00
checkout v2 202601010200.00
own B 202601010201.00; build B 202601010202.00
own B 202601010300.00; build B 202601010301.00        # B's next job sets src.txt back to the time B saw it at
own A 202601010400.00; build A 202601010401.00
if [ "$(holds A)" = v2 ]; then echo "source-times own, shared checkout: A rebuilt the change B saw first, as planted"
else fail "shared checkout: A holds $(holds A) after v2, not v2"; fi

# 2. A job that did not build (a part of the tree, a failure, a cut-off) leaves a record that is still true.
setup
own A 202601010100.00; build A 202601010101.00
checkout v2 202601010200.00
own A 202601010300.00                                  # built nothing that reads src.txt
own B 202601010301.00; build B 202601010302.00
own A 202601010400.00; build A 202601010401.00
if [ "$(holds A)" = v2 ]; then echo "source-times own, a job that built nothing: the next one rebuilds, as planted"
else fail "a job that built nothing: A holds $(holds A) after v2, not v2"; fi

# 3. Nothing changed: nothing is rebuilt, however the other directory moved the times.
setup
own A 202601010100.00; build A 202601010101.00
own B 202601010200.00; build B 202601010201.00
own A 202601010300.00; build A 202601010301.00
if [ "$(builds A)" = 1 ]; then echo "source-times own, nothing changed: no rebuild, as planted"
else fail "nothing changed: A built $(builds A) times, not once"; fi

# 4. No record: every source gets NOW.
setup
touch -t 200001010000.00 "$work/case/repo/src.txt"
own A 202601010500.00
if [ "$(cd "$work/case/repo" && find src.txt same.txt -newermt '2026-01-01 04:59:59' | wc -l | tr -d ' ')" = 2 ]; then
  echo "source-times own, no record: every source gets NOW, as planted"
else fail "no record: not every source got NOW"; fi

# 5. A NOW not in touch -t form is refused.
if (cd "$work/case/repo" && bash "$script" own "$work/case/A/.nexus-ci-times" 2026-01-01 > /dev/null 2>&1); then
  fail "a malformed NOW was taken"
else echo "source-times own, malformed NOW: refused, as planted"; fi

# Reversed witness: the same steps as case 1 through the old apply and record (fleet-build before this fix: apply
# when a record is there, then remove it; record after a build) leave A stale.
legacy() {
  local m="$work/case/$1/.nexus-ci-sources"
  if [ -f "$m" ]; then (cd "$work/case/repo" && bash "$script" apply "$m") > /dev/null; rm -f "$m"; fi
}
record() { (cd "$work/case/repo" && bash "$script" record "$work/case/$1/.nexus-ci-sources") > /dev/null; }
setup
legacy A; build A 202601010101.00; record A
checkout v2 202601010200.00
legacy B; build B 202601010202.00; record B
legacy B; build B 202601010301.00
legacy A; build A 202601010401.00
if [ "$(holds A)" = v1 ]; then echo "source-times apply and record, shared checkout: A stale, as on the runner (the witness)"
else fail "the witness: the old apply and record left A holding $(holds A); the case no longer reproduces the stale build"; fi

exit "$bad"
