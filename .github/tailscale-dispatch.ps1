# .github/tailscale-dispatch.sh for the Windows job (pwsh): on a machine whose key directory is not primed, sets the
# commit status tailscale-<OS> on SHA to pending, dispatches tailscale-prime.yml for that commit and OS (one slot, status
# on), and fails, since this job built and tested nothing. Environment: GH_TOKEN (actions: write and statuses: write),
# OS, DIR, REF, SHA, and the runner's GITHUB_*.
$ErrorActionPreference = 'Stop'
$status = Join-Path $PSScriptRoot 'tailscale-status.ps1'
$env:STATE = 'pending'
$env:DESCRIPTION = "$env:COMPUTERNAME is not primed for these pins: tailscale-prime builds and tests this commit"
$env:TARGET = "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/workflows/tailscale-prime.yml"
$global:LASTEXITCODE = 0
& $status
if ($LASTEXITCODE -ne 0) {
  "::error::setting tailscale-$env:OS to pending on $env:SHA was not taken, so nothing was dispatched: this commit is not built on $env:OS"
  exit 1
}
$body = @{ ref = $env:REF; inputs = @{ os = $env:OS; sha = $env:SHA; status = 'true' } } | ConvertTo-Json -Compress
$code = 0
try {
  $r = Invoke-WebRequest -Method Post -Body $body -ContentType 'application/json' -SkipHttpErrorCheck `
    -Headers @{ Authorization = "Bearer $env:GH_TOKEN"; Accept = 'application/vnd.github+json' } `
    -Uri "https://api.github.com/repos/$env:GITHUB_REPOSITORY/actions/workflows/tailscale-prime.yml/dispatches"
  $code = [int]$r.StatusCode
} catch {
  "::warning::the dispatch of tailscale-prime did not reach GitHub: $($_.Exception.Message)"
}
if ($code -ne 204) {
  $env:STATE = 'error'
  $env:DESCRIPTION = "$env:COMPUTERNAME is not primed, and dispatching tailscale-prime answered HTTP $code"
  $env:TARGET = "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
  & $status
  "::error::$env:COMPUTERNAME has no primed caches for these pins ($env:DIR), and dispatching tailscale-prime answered HTTP ${code}: this commit is not built on $env:OS"
  exit 1
}
"::error::$env:COMPUTERNAME has no primed caches for these pins ($env:DIR), and a cold build does not fit this job's 2 minutes: this job built and tested nothing, so it fails. tailscale-prime ($env:OS) builds and tests $env:SHA with no limit and sets the commit status tailscale-$env:OS to its result, which is this commit's verdict on $env:OS"
exit 1
