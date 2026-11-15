# The parallelism for a CI job's compiles (ci.yml and nightly.yml, as `JOBS=$(sh .github/job-cores.sh)`; tests keep
# the machine's core count, TEST_JOBS, and ci.yml's opening comment says why): the runner's share of its host's cores,
# NOVA_JOB_CORES, which each fleet runner exports, so that compiles sharing a host do not each ask for the whole
# machine; the machine's core count where it is unset or not a positive integer. Prints the
# number, and says on stderr (the step's log) which it used. Windows does the same in pwsh (ci.yml, windows).
cores=${NOVA_JOB_CORES:-}
case "$cores" in
  '' | *[!0-9]* | 0*)
    if command -v nproc >/dev/null 2>&1; then
      cores=$(nproc)
    else
      cores=$(sysctl -n hw.ncpu)
    fi
    echo "NOVA_JOB_CORES on $(hostname -s) is '${NOVA_JOB_CORES-unset}', not a positive integer: jobs use the core count, $cores" >&2
    ;;
  *)
    echo "NOVA_JOB_CORES on $(hostname -s) is $cores: jobs use it" >&2
    ;;
esac
echo "$cores"
