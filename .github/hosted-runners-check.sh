#!/usr/bin/env bash
# Refuses a GitHub-hosted runner in nexus's per-commit CI (Glenn, 2026-10-18: "all CI runs on our fleet's self-hosted
# runners, never GitHub-hosted runners"; 2026-11-26: "Yes, fleet runners"; ci.yml's header).
#
# In scope: every workflow under .github/workflows that runs on a push or a pull request (push, pull_request,
# pull_request_target, merge_group), and every workflow such a workflow calls (`uses: ./.github/workflows/...`), to any
# depth. In each, every job's runs-on must be a literal list that starts with self-hosted (`runs-on: [self-hosted, ...]`):
# a hosted label (ubuntu-*, macos-*, windows-*), a bare label, or an expression (which could name a hosted one) is
# refused, by file and line. A workflow out of scope that still names a hosted runner (the nightly's last hosted jobs)
# is listed as a warning, by file and line, so it is never out of sight.
#
#   bash .github/hosted-runners-check.sh [ROOT]   checks ROOT/.github/workflows (ROOT defaults to .); exits 1 on a refusal
#   bash .github/hosted-runners-check.sh --selftest   runs it against its planted cases (tests/ci/hosted-runners): good
#                                                     passes, and hosted, expression, called and bare each fail
set -uo pipefail

check() {
  local root=$1 dir="$1/.github/workflows" bad=0 f name
  if [ ! -d "$dir" ]; then echo "::error::no $dir"; return 1; fi
  local -a files=() scope=() queue=()
  while IFS= read -r f; do files+=("$f"); done < <(find "$dir" -maxdepth 1 -type f \( -name '*.yml' -o -name '*.yaml' \) | LC_ALL=C sort)
  if [ ${#files[@]} -eq 0 ]; then echo "::error::no workflows in $dir"; return 1; fi
  # Per-commit triggers: a trigger key under the top-level `on:` block, or an inline `on: [...]` / `on: push`.
  for f in "${files[@]}"; do
    if awk '
        /^on:[[:space:]]*\[/ { if ($0 ~ /(push|pull_request|pull_request_target|merge_group)/) found=1; next }
        /^on:[[:space:]]*[a-z_]+[[:space:]]*$/ { if ($2 ~ /^(push|pull_request|pull_request_target|merge_group)$/) found=1; next }
        /^on:[[:space:]]*$/ { inon=1; next }
        /^[^[:space:]#]/ { inon=0 }
        inon && /^  (push|pull_request|pull_request_target|merge_group)[[:space:]]*:/ { found=1 }
        END { exit found ? 0 : 1 }' "$f"; then
      queue+=("$f")
    fi
  done
  # Workflows the per-commit ones call, to any depth.
  while [ ${#queue[@]} -gt 0 ]; do
    f=${queue[0]}; queue=("${queue[@]:1}")
    case " ${scope[*]-} " in *" $f "*) continue ;; esac
    scope+=("$f")
    while IFS= read -r name; do
      if [ -f "$dir/$name" ]; then queue+=("$dir/$name"); else echo "::error file=$f::calls ./.github/workflows/$name, which does not exist"; bad=1; fi
    done < <(sed -n 's#^[[:space:]]*uses:[[:space:]]*\./\.github/workflows/\([^[:space:]@]*\).*#\1#p' "$f")
  done
  if [ ${#scope[@]} -eq 0 ]; then echo "::error::no workflow in $dir runs on a push or a pull request"; return 1; fi
  local line text value
  for f in "${files[@]}"; do
    local inscope=0
    case " ${scope[*]} " in *" $f "*) inscope=1 ;; esac
    while IFS=: read -r line text; do
      value=$(printf '%s' "$text" | sed -E 's/^[[:space:]]*runs-on:[[:space:]]*//; s/[[:space:]]+#.*$//; s/[[:space:]]+$//')
      if [ "$inscope" = 1 ]; then
        if [[ "$value" =~ ^\[[[:space:]]*self-hosted[[:space:]]*(,|\]) ]] && ! [[ "$value" =~ (ubuntu-|macos-|windows-[0-9]|-latest|\$\{\{) ]]; then
          continue
        fi
        echo "::error file=${f#"$root"/},line=$line::runs-on: $value is not a fleet runner: a per-commit job runs on [self-hosted, ...] labels only, never a GitHub-hosted runner or an expression that could name one"
        bad=1
      elif [[ "$value" =~ (ubuntu-|macos-|windows-[0-9]|-latest) ]]; then
        echo "::warning file=${f#"$root"/},line=$line::runs-on: $value is a GitHub-hosted runner (not a per-commit workflow, so not refused here)"
      fi
    done < <(grep -n -E '^[[:space:]]*runs-on:' "$f")
  done
  if [ "$bad" = 0 ]; then
    echo "every runs-on in ${#scope[@]} per-commit workflow(s) is a fleet runner: $(for f in "${scope[@]}"; do printf '%s ' "${f##*/}"; done)"
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
  for case in hosted expression called bare; do
    if out=$(check "$planted/$case" 2>&1); then
      echo "::error title=hosted-runners-check self-test::the $case case passed; it should fail"; echo "$out"; bad=1
    else
      echo "hosted-runners-check, $case: refused, as planted ($(printf '%s\n' "$out" | grep -m1 -o 'runs-on: [^:]*is not a fleet runner'))"
    fi
  done
  exit "$bad"
fi
check "${1:-.}"
