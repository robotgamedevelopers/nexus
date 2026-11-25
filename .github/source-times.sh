#!/usr/bin/env bash
# Source file times for a build directory restored from a cache (.github/actions/hosted-caches, windows-build).
# A checkout gives every file the time of the checkout, so make, ninja, MSBuild and cargo would rebuild everything the
# cache already holds. Times are set by content instead:
#   apply  MANIFEST  after a restore (a per-commit job, and cache-warm when it restored an older cache): a file
#                    whose blob is the one MANIFEST records (the very content the cache was built from) gets a fixed
#                    old time; a changed or new file keeps the checkout's time, so exactly what changed, and what
#                    depends on it, is rebuilt.
#   record MANIFEST  cache-warm, after its build: the blobs the build was made from, saved with the cache.
# A file is made old only when its content is byte for byte what the cache was built from, so nothing stale is kept.
set -euo pipefail
mode=${1:?usage: source-times.sh apply|record MANIFEST}
manifest=${2:?usage: source-times.sh apply|record MANIFEST}
old=200101010000
current=$(mktemp)
trap 'rm -f "$current"' EXIT
git ls-files -s | LC_ALL=C sort > "$current"
case "$mode" in
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
