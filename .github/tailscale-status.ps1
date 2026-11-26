# .github/tailscale-status.sh for the Windows job (pwsh): sets the commit status tailscale-<OS> on SHA to STATE
# (pending, success, failure or error) with DESCRIPTION and the link TARGET, and fails if GitHub does not take it.
# Environment: GH_TOKEN (statuses: write), OS, SHA, STATE, DESCRIPTION, TARGET, and the runner's GITHUB_REPOSITORY.
$ErrorActionPreference = 'Stop'
$description = $env:DESCRIPTION
if ($description.Length -gt 140) { $description = $description.Substring(0, 140) }
$body = @{ state = $env:STATE; context = "tailscale-$env:OS"; description = $description; target_url = $env:TARGET } |
  ConvertTo-Json -Compress
$code = 0
# GitHub answers 5xx now and then: three tries, each said, two seconds apart; any other answer is final.
foreach ($attempt in 1..3) {
  try {
    $r = Invoke-WebRequest -Method Post -Body $body -ContentType 'application/json' -SkipHttpErrorCheck `
    -Headers @{ Authorization = "Bearer $env:GH_TOKEN"; Accept = 'application/vnd.github+json' } `
    -Uri "https://api.github.com/repos/$env:GITHUB_REPOSITORY/statuses/$env:SHA"
    $code = [int]$r.StatusCode
  } catch {
    "::error::setting the status tailscale-$env:OS did not reach GitHub: $($_.Exception.Message)"
    exit 1
  }
  if ($code -lt 500) { break }
  "::warning::setting the status tailscale-$env:OS: try $attempt answered HTTP $code"
  if ($attempt -lt 3) { Start-Sleep -Seconds 2 }
}
if ($code -ne 201) {
  "::error::setting the status tailscale-$env:OS ($env:STATE) on $env:SHA answered HTTP $code"
  exit 1
}
"tailscale-$env:OS on ${env:SHA}: $env:STATE ($description)"
# Said explicitly: a caller that runs this in its own session (& .github/tailscale-status.ps1) reads $LASTEXITCODE,
# which a script that ends without exit leaves as it was ($null in a new session, which is not 0).
exit 0
