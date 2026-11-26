# ctest for CI on Windows, as ctest.sh (which says why): a selection that runs no test fails, and so does a skipped or
# disabled test.
$log = New-TemporaryFile
try {
  & ctest @args --no-tests=error 2>&1 | Tee-Object -FilePath $log
  $rc = $LASTEXITCODE
  if (Select-String -Path $log -Pattern '^The following tests did not run:' -Quiet) {
    Write-Output "::error title=A test did not run::a test was skipped or disabled, and no test is ever skipped: make it run, or move it to the tier that can run it"
    exit 1
  }
  exit $rc
} finally {
  Remove-Item -Force $log
}
