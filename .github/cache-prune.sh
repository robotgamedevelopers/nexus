#!/usr/bin/env bash
# Deletes the older caches of one name on main after cache-warm saved a new one (.github/workflows/cache-warm.yml):
# every cache on refs/heads/main whose key starts with "<name>-v" (any version, any Prax pin), except the one just
# saved. Pull requests restore the newest, so only it is kept; the repository's caches share one quota.
# Needs GH_TOKEN with actions: write. Every failure is loud: a list or delete that fails fails the job.
set -euo pipefail
name=${1:?usage: cache-prune.sh NAME KEEP_KEY}
keep=${2:?usage: cache-prune.sh NAME KEEP_KEY}
case "$keep" in
  "$name"-v*) ;;
  *) echo "::error::cache-prune.sh: the key to keep ($keep) is not one of $name's"; exit 1 ;;
esac
ids=$(gh api --paginate "repos/$GITHUB_REPOSITORY/actions/caches?ref=refs/heads/main&key=$name-v&per_page=100" \
  --jq ".actions_caches[] | select(.key != \"$keep\") | select(.key | startswith(\"$name-v\")) | \"\(.id) \(.key)\"")
if ! gh api "repos/$GITHUB_REPOSITORY/actions/caches?ref=refs/heads/main&key=$keep" --jq '.actions_caches[].key' | grep -qxF "$keep"; then
  echo "::error::cache-prune.sh: the cache just saved ($keep) is not listed on main, so nothing is deleted"
  exit 1
fi
deleted=0
while read -r id key; do
  [ -n "$id" ] || continue
  gh api -X DELETE "repos/$GITHUB_REPOSITORY/actions/caches/$id" >/dev/null
  echo "deleted $key"
  deleted=$((deleted + 1))
done <<< "$ids"
echo "kept $keep; deleted $deleted older cache(s) of $name"
