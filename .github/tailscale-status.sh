#!/usr/bin/env bash
# Sets the commit status tailscale-<OS> on SHA (.github/workflows/tailscale.yml says what the statuses mean): STATE is
# pending, success, failure or error; DESCRIPTION is shown beside it (cut to GitHub's 140 characters); TARGET is its
# link. Fails the step if GitHub does not take it, since a status that was not set must not read as one that was.
# Environment: GH_TOKEN (statuses: write), OS, SHA, STATE, DESCRIPTION, TARGET, and the runner's GITHUB_REPOSITORY.
set -euo pipefail

description=${DESCRIPTION:0:140}
description=${description//\\/\\\\}
description=${description//\"/\\\"}
body="{\"state\":\"$STATE\",\"context\":\"tailscale-$OS\",\"description\":\"$description\",\"target_url\":\"$TARGET\"}"
# GitHub answers 5xx now and then: three tries, each said, two seconds apart; any other answer is final.
for attempt in 1 2 3; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
    -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
    "https://api.github.com/repos/$GITHUB_REPOSITORY/statuses/$SHA" -d "$body") || code=000
  case "$code" in 5??|000) echo "::warning::setting the status tailscale-$OS: try $attempt answered HTTP $code" ;; *) break ;; esac
  if [ "$attempt" -lt 3 ]; then sleep 2; fi
done
if [ "$code" != 201 ]; then
  echo "::error::setting the status tailscale-$OS ($STATE) on $SHA answered HTTP $code"
  exit 1
fi
echo "tailscale-$OS on $SHA: $STATE ($description)"
