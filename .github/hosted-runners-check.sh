#!/usr/bin/env bash
# Refuses a GitHub-hosted runner anywhere in nexus's CI (Glenn, 2026-10-18: "all CI runs on our fleet's self-hosted
# runners, never GitHub-hosted runners"; 2026-11-26: "Yes, fleet runners"; ci.yml's header).
#
# Every workflow under .github/workflows is in scope: the per-commit ones, the ones they call, the nightly, cache-warm
# and anything run by hand. In each, every job's runs-on must be a literal list that starts with self-hosted
# (`runs-on: [self-hosted, ...]`): a hosted label (ubuntu-*, macos-*, windows-*), a bare label, or an expression (which
# could name a hosted one) is refused, by file and line. A `uses: ./.github/workflows/...` that names no file is refused
# too, so a called workflow is never out of sight.
#
#   bash .github/hosted-runners-check.sh [ROOT]   checks ROOT/.github/workflows (ROOT defaults to .); exits 1 on a refusal
#   bash .github/hosted-runners-check.sh --selftest   runs it against its planted cases (tests/ci/hosted-runners): good
#                                                     passes, and hosted, expression, called, bare and nightly each fail
set -uo pipefail

check() {
  local root=$1 dir="$1/.github/workflows" bad=0 f name
  if [ ! -d "$dir" ]; then echo "::error::no $dir"; return 1; fi
  local -a files=()
  while IFS= read -r f; do files+=("$f"); done < <(find "$dir" -maxdepth 1 -type f \( -name '*.yml' -o -name '*.yaml' \) | LC_ALL=C sort)
  if [ ${#files[@]} -eq 0 ]; then echo "::error::no workflows in $dir"; return 1; fi
  local line text value jobs=0
  for f in "${files[@]}"; do
    while IFS= read -r name; do
      if [ ! -f "$dir/$name" ]; then echo "::error file=${f#"$root"/}::calls ./.github/workflows/$name, which does not exist"; bad=1; fi
    done < <(sed -n 's#^[[:space:]]*uses:[[:space:]]*\./\.github/workflows/\([^[:space:]@]*\).*#\1#p' "$f")
    while IFS=: read -r line text; do
      jobs=$((jobs + 1))
      value=$(printf '%s' "$text" | sed -E 's/^[[:space:]]*runs-on:[[:space:]]*//; s/[[:space:]]+#.*$//; s/[[:space:]]+$//')
      if [[ "$value" =~ ^\[[[:space:]]*self-hosted[[:space:]]*(,|\]) ]] && ! [[ "$value" =~ (ubuntu-|macos-|windows-[0-9]|-latest|\$\{\{) ]]; then
        continue
      fi
      echo "::error file=${f#"$root"/},line=$line::runs-on: $value is not a fleet runner: every job runs on [self-hosted, ...] labels only, never a GitHub-hosted runner or an expression that could name one"
      bad=1
    done < <(grep -n -E '^[[:space:]]*runs-on:' "$f")
  done
  if [ "$jobs" = 0 ]; then echo "::error::no runs-on at all in $dir"; return 1; fi
  if [ "$bad" = 0 ]; then
    echo "all $jobs runs-on lines in ${#files[@]} workflow(s) are fleet runners: $(for f in "${files[@]}"; do printf '%s ' "${f##*/}"; done)"
  fi
  return "$bad"
}

if [ "${1-}" = --selftest ]; then
  bad=0
  planted=tests/ci/hosted-runners
  if out=$(check "$planted/good" 2>&1); then
    echo "hosted-runners-check, good: passes, as planted"
  else
    echo "::error title=hosted-runners-check self-test::the good case failed"; echo "$out"; bad=1
  fi
  for case in hosted expression called bare nightly; do
    if out=$(check "$planted/$case" 2>&1); then
      echo "::error title=hosted-runners-check self-test::the $case case passed; it should fail"; echo "$out"; bad=1
    else
      echo "hosted-runners-check, $case: refused, as planted ($(printf '%s\n' "$out" | grep -m1 -o 'runs-on: [^:]*is not a fleet runner'))"
    fi
  done
  exit "$bad"
fi
check "${1:-.}"
