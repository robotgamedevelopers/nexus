# TLC's worker count and the JVM's flags for one run, shared by tests/model/tlc.cmake and tests/store/tlc.cmake.
# In: WORKERS, the instance's worker count (its tlc_test or store_tlc_test call; 2 unless given), and the environment
# variable NEXUS_TLC_CORES, the cores this CI job may use (ci.yml's model job sets it from the runner's share at run
# time; unset in a login session). Out: tlc_workers, the count TLC runs with, and tlc_jvm, the JVM's flags.
#
# The run takes at most NEXUS_TLC_CORES workers: more workers than the job's cores do not finish sooner on a shared
# host, they spend the job's share on contention (MCSupervisorSwitch.cfg: 162 CPU s on four workers, 288 on eight, the
# same 45-49 s). The JIT and collector follow the instance's own size, WORKERS, not the cut count: a large instance cut
# to two workers keeps the optimising compiler, which it needs (MCSupervisorSwitch.cfg on two workers under the client
# compiler alone ran past 300 s; see the CMakeLists beside each instance for its times).

if(NOT DEFINED WORKERS OR "${WORKERS}" STREQUAL "")
    set(WORKERS 2)
endif()
set(tlc_workers ${WORKERS})
set(job_cores "$ENV{NEXUS_TLC_CORES}")
if(job_cores MATCHES "^[1-9][0-9]*$" AND tlc_workers GREATER job_cores)
    message("TLC: ${WORKERS} workers cut to ${job_cores}, this job's cores (NEXUS_TLC_CORES)")
    set(tlc_workers ${job_cores})
endif()
# The JVM's own threads, sized to the run. Left alone, each JVM starts a garbage collector thread for most cores and
# several compiler threads, and ctest runs one JVM per core: on a busy machine those threads, not TLC, use the cores.
if(WORKERS GREATER 2)
    set(tlc_jvm -XX:+UseParallelGC -XX:ParallelGCThreads=${tlc_workers})
else()
    # A witness or a trace check runs for a second or two: the client compiler alone and the serial collector.
    set(tlc_jvm -XX:+UseSerialGC -XX:TieredStopAtLevel=1)
endif()
