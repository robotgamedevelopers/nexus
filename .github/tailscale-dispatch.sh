#!/usr/bin/env bash
# A per-commit Tailscale job on a machine whose cache key directory is not primed (.github/workflows/tailscale.yml):
# sets the commit status tailscale-<OS> on SHA to pending, linked to tailscale-prime's runs, and dispatches
# tailscale-prime.yml for that commit and OS (one slot, status on), which sets the status to its result. Fails the
# step if either is not taken. Environment: GH_TOKEN (actions: write and statuses: write), OS, DIR (the key
# directory), REF (the branch), SHA, and the runner's GITHUB_*.
set -euo pipefail

host=$(hostname -s)
STATE=pending \
  DESCRIPTION="$host is not primed for these pins: tailscale-prime builds and tests this commit" \
  TARGET="$GITHUB_SERVER_URL/$GITHUB_REPOSITORY/actions/workflows/tailscale-prime.yml" \
  bash .github/tailscale-status.sh
body="{\"ref\":\"$REF\",\"inputs\":{\"os\":\"$OS\",\"sha\":\"$SHA\",\"status\":\"true\"}}"
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
  "https://api.github.com/repos/$GITHUB_REPOSITORY/actions/workflows/tailscale-prime.yml/dispatches" \
  -d "$body") || code=000
if [ "$code" != 204 ]; then
  STATE=error DESCRIPTION="$host is not primed, and dispatching tailscale-prime answered HTTP $code" \
    TARGET="$GITHUB_SERVER_URL/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID" bash .github/tailscale-status.sh
  echo "::error::$host has no primed caches for these pins ($DIR), and dispatching tailscale-prime answered HTTP $code: this commit is not built on $OS"
  exit 1
fi
echo "::error::$host has no primed caches for these pins ($DIR), and a cold build does not fit this job's 2 minutes: this job built and tested nothing, so it fails. tailscale-prime ($OS) builds and tests $SHA with no limit and sets the commit status tailscale-$OS to its result, which is this commit's verdict on $OS"
exit 1
