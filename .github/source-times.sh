#!/usr/bin/env bash
# Source file times, so that make, ninja, MSBuild and cargo rebuild exactly what changed.
#
#   own    TIMES [NOW]  a fleet runner's build directory (.github/actions/fleet-build, windows-build), before its build:
#                       the sources get the times this directory last saw them with. A file whose blob is the one
#                       TIMES records for it gets back the time TIMES records; a changed or new file gets NOW (touch -t
#                       form, CCYYMMDDhhmm.SS; the clock when not given). TIMES is then written with what was set, before
#                       the build, so a build that fails, is cut off or builds part of the tree (ci.yml's
#                       tier_pr_build) leaves a record that is still true. With no TIMES every file gets NOW, and the
#                       directory builds again whole (ccache, shared by the machine's runners, makes that mostly hits).
#                       Every build directory a runner keeps shares the runner's one checkout, and each job changes
#                       that checkout's times; so a directory trusts no time it did not set itself. Each directory thus
#                       sees its sources as a checkout of its own would show them: a file changed since this directory
#                       last built is newer than everything the directory holds.
#   apply  MANIFEST     a build directory restored from GitHub's cache onto a fresh checkout (.github/actions/hosted-
#                       caches): a file whose blob is the one MANIFEST records gets a fixed old time; a changed or new
#                       file keeps the checkout's time. Only for a checkout no other build directory shares.
#   record MANIFEST     the blobs a build of everything was made from, saved with that cache.
#   --selftest          own's planted cases (tests/ci/source-times-test.sh).
set -euo pipefail
if [ "${1-}" = --selftest ]; then
  exec bash "$(dirname "$0")/../tests/ci/source-times-test.sh" "$0"
fi
mode=${1:?usage: source-times.sh own TIMES [NOW] | apply MANIFEST | record MANIFEST | --selftest}
manifest=${2:?usage: source-times.sh own TIMES [NOW] | apply MANIFEST | record MANIFEST | --selftest}
old=200101010000
current=$(mktemp)
plan=$(mktemp)
changed=$(mktemp)
trap 'rm -f "$current" "$plan" "$changed"' EXIT
git ls-files -s | LC_ALL=C sort > "$current"
# git quotes a path with unusual bytes; such a path would be touched under the wrong name.
if grep -q "$(printf '\t')\"" "$current"; then
  echo "::error::source-times.sh: a tracked path git quotes (unusual bytes in its name); rename it:"
  grep "$(printf '\t')\"" "$current" | cut -f2- | sed -n 1,10p
  exit 1
fi
case "$mode" in
  own)
    now=${3:-$(date +%Y%m%d%H%M.%S)}
    if ! printf '%s\n' "$now" | grep -Eq '^[0-9]{12}\.[0-9]{2}$'; then
      echo "::error::source-times.sh own: NOW is '$now', not CCYYMMDDhhmm.SS"
      exit 1
    fi
    if [ -f "$manifest" ]; then
      # Each line of TIMES: git ls-files -s's line (mode blob stage, a tab, the path), a tab, the time set.
      awk -F'\t' -v now="$now" -v changed="$changed" '
        NR == FNR { t[$1 FS $2] = $3; next }
        { k = $1 FS $2; if (k in t) print k FS t[k]; else { print k FS now; print $2 > changed } }' "$manifest" "$current" > "$plan"
    else
      awk -F'\t' -v now="$now" '{ print $1 FS $2 FS now }' "$current" > "$plan"
      cut -f2- "$current" > "$changed"
      echo "no record of this build directory's source times at $manifest: every source gets $now, and the directory builds whole"
    fi
    # One touch per distinct time.
    LC_ALL=C sort -t "$(printf '\t')" -k3,3 "$plan" | awk -F'\t' '
      $3 != last { if (NR > 1) printf "\n"; printf "%s", $3; last = $3 }
      { printf "\t%s", $2 }
      END { if (NR > 0) printf "\n" }' | while IFS= read -r line; do
        stamp=${line%%$'\t'*}
        printf '%s' "${line#*$'\t'}" | tr '\t' '\0' | xargs -0 touch -h -t "$stamp"
      done
    mkdir -p "$(dirname "$manifest")"
    cp "$plan" "$manifest.new"
    mv "$manifest.new" "$manifest"
    n=$(wc -l < "$changed" | tr -d ' ')
    echo "$(( $(wc -l < "$plan") - n )) of $(wc -l < "$plan" | tr -d ' ') tracked files as this build directory last saw them; $n changed or new since, at $now:"
    sed -n 1,50p "$changed"
    ;;
  record)
    mkdir -p "$(dirname "$manifest")"
    cp "$current" "$manifest"
    echo "recorded the blobs of $(wc -l < "$current" | tr -d ' ') tracked files in $manifest"
    ;;
  apply)
    if [ ! -f "$manifest" ]; then
      echo "::error::no source manifest at $manifest in the restored cache: run the cache-warm workflow on main"
      exit 1
    fi
    same=$(mktemp)
    LC_ALL=C comm -12 "$manifest" "$current" | cut -f2- > "$same"
    if [ -s "$same" ]; then
      tr '\n' '\0' < "$same" | xargs -0 touch -h -t "$old"
    fi
    echo "$(wc -l < "$same" | tr -d ' ') of $(wc -l < "$current" | tr -d ' ') tracked files as the cache built them; the rest are rebuilt"
    LC_ALL=C comm -13 "$manifest" "$current" | cut -f2- | sed -n 1,50p
    rm -f "$same"
    ;;
  *) echo "::error::source-times.sh: unknown mode '$mode'"; exit 1 ;;
esac
