# Tests of the server's durable state (src/store/), with four labels:
#   unit           store_test: the journal, the store and MemFs, in memory, crashes injected at every write and sync;
#                  no time, no sockets, no files (one ctest entry per group, so they run in parallel); durable_test:
#                  nexus-server's Hub over the store the same way (a restart, a reconnecting window's resent actions,
#                  a crash at every write and sync)
#   functional     store_functional_test: the store on the real filesystem in its own working directory, a process
#                  killed while it commits and the store reopened
#   model          TLC: models/store/Journal.tla on two instances, every reversed witness rejected, code traces
#                  (journal_walk) checked against the spec over several seeds, and faulty journals' traces rejected.
#                  Needs TLA2TOOLS_JAR and java.
#   model_nightly  TLC: the largest instance (the nightly workflow)

set(warnings ${NEXUS_WARNINGS})

add_executable(store_test store_test.cpp)
target_link_libraries(store_test PRIVATE nexus_store)
target_compile_options(store_test PRIVATE ${warnings})

add_executable(durable_test durable_test.cpp)
target_link_libraries(durable_test PRIVATE nexus_durable)
target_compile_options(durable_test PRIVATE ${warnings})

add_executable(journal_walk journal_walk.cpp)
target_link_libraries(journal_walk PRIVATE nexus_store)
target_compile_options(journal_walk PRIVATE ${warnings})

# The store on the real filesystem: POSIX, like nexus-server (not built natively on Windows, where nexus-server runs inside WSL2: CMakeLists.txt NEXUS_LINK).
if(NOT WIN32)
    add_executable(store_functional_test store_functional_test.cpp)
    target_link_libraries(store_functional_test PRIVATE nexus_store)
    target_compile_options(store_functional_test PRIVATE ${warnings})
endif()

# The programs the unit and model tests run, built with the model's (cmake --build <dir> --target model_checks), so
# the sanitizer and model jobs build them without the app.
add_custom_target(store_checks DEPENDS store_test durable_test journal_walk)
add_dependencies(model_checks store_checks)

foreach(group codec memfs journal journal_crash store ids store_crash driver migrate dequeue formats)
    add_test(NAME store_unit_${group} COMMAND store_test ${group})
    set_tests_properties(store_unit_${group} PROPERTIES LABELS unit TIMEOUT 60)
endforeach()
foreach(group restart resend crash take migrate kinds)
    add_test(NAME durable_unit_${group} COMMAND durable_test ${group})
    set_tests_properties(durable_unit_${group} PROPERTIES LABELS unit TIMEOUT 60)
endforeach()

if(NOT WIN32)
    set(functional_dir "${CMAKE_CURRENT_BINARY_DIR}/functional")
    file(MAKE_DIRECTORY "${functional_dir}")
    add_test(NAME store_functional COMMAND store_functional_test WORKING_DIRECTORY "${functional_dir}")
    set_tests_properties(store_functional PROPERTIES LABELS functional TIMEOUT 60)
endif()

set(store_models "${PROJECT_SOURCE_DIR}/models/store")
set(store_tlc "${CMAKE_CURRENT_SOURCE_DIR}/tlc.cmake")

# store_tlc_test(<name> <module> <cfg> <expect> [PROPERTY p] [WALK args...] [WORKERS w] [NIGHTLY])
function(store_tlc_test name module cfg expect)
    cmake_parse_arguments(T "NIGHTLY" "PROPERTY;WORKERS" "WALK" ${ARGN})
    if(NOT T_WORKERS)
        set(T_WORKERS 4)
    endif()
    set(label model)
    set(cap 60)
    if(T_NIGHTLY)
        set(label model_nightly)
        set(cap 300)
    endif()
    math(EXPR ctest_timeout "${cap} + 30")
    set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${store_models} -DMODULE=${module} -DCFG=${cfg}
             -DEXPECT=${expect} -DWORKERS=${T_WORKERS} -DCAP=${cap})
    if(T_PROPERTY)
        list(APPEND defs -DPROPERTY=${T_PROPERTY})
    endif()
    if(T_WALK)
        string(REPLACE ";" "," walk_args "${T_WALK}")
        list(APPEND defs -DWALK=$<TARGET_FILE:journal_walk> -DWALK_ARGS=${walk_args})
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${store_tlc})
    set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT ${ctest_timeout} PROCESSORS ${T_WORKERS})
endfunction()

# The spec, every invariant, no error: four writes (310k states), and records of two lengths. Four TLC workers each
# (ctest counts them as four of its slots), eight on the nightly instance.
store_tlc_test(tlc_journal_spec   Journal Journal.cfg     pass)
store_tlc_test(tlc_journal_lens   Journal JournalLens.cfg pass)
store_tlc_test(tlc_journal_large  Journal JournalLarge.cfg pass WORKERS 8 NIGHTLY)

# Reversed witnesses: each broken variant must be rejected by the invariant named.
store_tlc_test(tlc_journal_witness_accept_torn    Journal witnesses/JAcceptTorn.cfg       invariant PROPERTY NoTornAccepted)
store_tlc_test(tlc_journal_witness_cut_too_far    Journal witnesses/JCutTooFar.cfg        invariant PROPERTY NoAckedLost)
store_tlc_test(tlc_journal_witness_no_truncate    Journal witnesses/JNoTruncate.cfg       invariant PROPERTY NoAckedLost)
store_tlc_test(tlc_journal_witness_ack_before_sync Journal witnesses/JAckBeforeSync.cfg   invariant PROPERTY NoAckedLost)
store_tlc_test(tlc_journal_witness_rename_before_sync Journal witnesses/JRenameBeforeSync.cfg invariant
               PROPERTY NotBroken)
store_tlc_test(tlc_journal_witness_no_dir_sync    Journal witnesses/JNoDirSync.cfg        invariant PROPERTY NoAckedLost)
store_tlc_test(tlc_journal_witness_compact_no_sync Journal witnesses/JCompactNoSync.cfg   invariant PROPERTY NotBroken)
store_tlc_test(tlc_journal_witness_no_skip        Journal witnesses/JNoSkip.cfg           invariant
               PROPERTY RecoveredWritten)
store_tlc_test(tlc_journal_witness_trunc_off_by_one Journal witnesses/JTruncOffByOne.cfg  invariant PROPERTY CutExact)

# Trace binding: the real journal's random walks on MemFs, each a behaviour of Journal.tla.
foreach(seed 1 2 3 4 5)
    store_tlc_test(tlc_journal_trace_seed_${seed} JournalTrace JournalTrace.cfg pass WALK ${seed} 1000)
endforeach()

# The binding can fail: a faulty journal's trace must be rejected. A recovery that keeps a torn record or cuts too far
# decides differently from the spec (TraceMatches); one that leaves the torn tail, or a sync that does not sync, takes
# a step the spec cannot (deadlock).
store_tlc_test(tlc_journal_trace_accept_torn_rejected JournalTrace JournalTrace.cfg invariant PROPERTY TraceMatches
               WALK 1 1000 --fault accept-torn)
store_tlc_test(tlc_journal_trace_cut_too_far_rejected JournalTrace JournalTrace.cfg invariant PROPERTY TraceMatches
               WALK 1 1000 --fault cut-too-far)
store_tlc_test(tlc_journal_trace_no_truncate_rejected JournalTrace JournalTrace.cfg deadlock
               WALK 1 1000 --fault no-truncate)
store_tlc_test(tlc_journal_trace_no_sync_rejected     JournalTrace JournalTrace.cfg deadlock
               WALK 1 1000 --fault no-sync)
