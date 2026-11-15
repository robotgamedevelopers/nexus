#!/usr/bin/env bash
# A per-commit Tailscale job on a machine whose cache key directory is not primed (.github/workflows/tailscale.yml):
# dispatch tailscale-prime.yml for this commit and OS, and say so. Sets the step output dispatched=true when GitHub
# took the dispatch (HTTP 204), so the job builds nothing; otherwise dispatched=false, with a warning, and the job builds
# under its 2 minutes (the dispatch answers 404 while tailscale-prime.yml is not on the default branch).
# Environment: GH_TOKEN, OS, DIR (the key directory), REF (the branch), SHA, and the runner's GITHUB_*.
set -euo pipefail

body="{\"ref\":\"$REF\",\"inputs\":{\"os\":\"$OS\",\"sha\":\"$SHA\"}}"
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
  "https://api.github.com/repos/$GITHUB_REPOSITORY/actions/workflows/tailscale-prime.yml/dispatches" \
  -d "$body") || code=000
host=$(hostname -s)
if [ "$code" = 204 ]; then
  echo "dispatched=true" >> "$GITHUB_OUTPUT"
  echo "::notice::$host has no primed caches for these pins ($DIR), and a cold build does not fit this job's 2 minutes: dispatched tailscale-prime ($OS) for $SHA, which builds and tests it with no limit; this job built nothing"
else
  echo "dispatched=false" >> "$GITHUB_OUTPUT"
  echo "::warning::$host has no primed caches for these pins ($DIR), and dispatching tailscale-prime answered HTTP $code (404 while the workflow is not on the default branch), so this job builds under its 2 minutes and a cold build may not finish"
fi
