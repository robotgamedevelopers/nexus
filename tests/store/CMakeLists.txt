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

# The store's lock and the server's start-up order (src/server/start_order.h) for several processes on one MemFs.
add_executable(store_lock_walk store_lock_walk.cpp)
target_link_libraries(store_lock_walk PRIVATE nexus_store)
target_compile_options(store_lock_walk PRIVATE ${warnings})

# The store on the real filesystem: POSIX, like nexus-server (not built natively on Windows, where nexus-server runs inside WSL2: CMakeLists.txt NEXUS_LINK).
if(NOT WIN32)
    add_executable(store_functional_test store_functional_test.cpp)
    target_link_libraries(store_functional_test PRIVATE nexus_store)
    target_compile_options(store_functional_test PRIVATE ${warnings})
endif()

# The programs the unit and model tests run, built with the model's (cmake --build <dir> --target model_checks), so
# the sanitizer and model jobs build them without the app.
add_custom_target(store_checks DEPENDS store_test durable_test journal_walk store_lock_walk)
add_dependencies(model_checks store_checks)

foreach(group codec memfs journal journal_crash store ids store_crash driver migrate dequeue formats instance lock
              start_order)
    add_test(NAME store_unit_${group} COMMAND store_test ${group})
    set_tests_properties(store_unit_${group} PROPERTIES LABELS unit TIMEOUT 60)
endforeach()
foreach(group restart resend crash batch batch_flush take migrate kinds)
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
    cmake_parse_arguments(T "NIGHTLY" "PROPERTY;WORKERS;WALKER" "WALK" ${ARGN})
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
        if(T_WALKER STREQUAL "store_lock_walk")
            list(APPEND defs -DWALK=$<TARGET_FILE:store_lock_walk> -DWALK_ARGS=${walk_args}
                 -DWALK_OUT=StoreLockTraceData.tla)
        else()
            list(APPEND defs -DWALK=$<TARGET_FILE:journal_walk> -DWALK_ARGS=${walk_args})
        endif()
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${store_tlc})
    set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT ${ctest_timeout} PROCESSORS ${T_WORKERS})
endfunction()

# The spec, every invariant, no error: four writes (310k states), and records of two lengths. Eight TLC workers on the
# spec (16 s on a loaded tlc runner host), four on the lengths (ctest counts each worker as one of its slots), eight on
# the nightly instance.
store_tlc_test(tlc_journal_spec   Journal Journal.cfg     pass WORKERS 8)
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

# The store's lock and the start (models/store/StoreLock.tla): two servers and a reader with Serves under fairness,
# two servers and a directory moved aside, three servers, two servers with failed steps (Serves and RefusedServes), and
# readers with no bound on reads (the gate: NoNewReaderWhileWriterWaits); every safety property.
store_tlc_test(tlc_store_lock_spec  StoreLock StoreLock.cfg      pass)
store_tlc_test(tlc_store_lock_move  StoreLock StoreLockMove.cfg  pass)
store_tlc_test(tlc_store_lock_three StoreLock StoreLockThree.cfg pass)
store_tlc_test(tlc_store_lock_fail  StoreLock StoreLockFail.cfg  pass)
store_tlc_test(tlc_store_lock_gate  StoreLock StoreLockGate.cfg  pass)
# Reversed witnesses: the order before the lock (two writers, a record written unlocked, a number written twice, the
# queued prompt sent twice); the drivers restored before the socket; writes by the store's name after it moved; a stop
# that removes the socket file its name names; a held lock taken as another server, and readers taken as one (Serves);
# a connection taken as a server serving (RefusedServes); and no gate (NoNewReaderWhileWriterWaits).
store_tlc_test(tlc_store_lock_witness_two_writers StoreLock witnesses/LUnlockedTwoWriters.cfg invariant
               PROPERTY OneWriter)
store_tlc_test(tlc_store_lock_witness_unlocked_writes StoreLock witnesses/LUnlockedWrites.cfg invariant
               PROPERTY LockedWrites)
store_tlc_test(tlc_store_lock_witness_seqs StoreLock witnesses/LUnlockedSeqs.cfg invariant PROPERTY SeqsUnique)
store_tlc_test(tlc_store_lock_witness_sent_twice StoreLock witnesses/LUnlockedSentTwice.cfg invariant
               PROPERTY SentOnce)
store_tlc_test(tlc_store_lock_witness_restore_first StoreLock witnesses/LRestoreFirst.cfg invariant
               PROPERTY ListenedWrites)
store_tlc_test(tlc_store_lock_witness_by_path StoreLock witnesses/LByPath.cfg invariant PROPERTY LockedWrites)
store_tlc_test(tlc_store_lock_witness_unlink_by_path StoreLock witnesses/LUnlinkByPath.cfg invariant
               PROPERTY SocketKept)
store_tlc_test(tlc_store_lock_witness_held_refused StoreLock witnesses/LHeldRefused.cfg temporal PROPERTY Serves)
store_tlc_test(tlc_store_lock_witness_reader_refused StoreLock witnesses/LReaderRefused.cfg temporal PROPERTY Serves)
store_tlc_test(tlc_store_lock_witness_connect_probe StoreLock witnesses/LConnectProbe.cfg temporal
               PROPERTY RefusedServes)
store_tlc_test(tlc_store_lock_witness_no_gate StoreLock witnesses/LNoGate.cfg action
               PROPERTY NoNewReaderWhileWriterWaits)
# Trace binding (functional: real files and the system's locks, in the test's own directory): the real StartOrder
# through take_step, Store::open's lock through PosixFs, and a reader's shared lock; each walk a behaviour of
# StoreLock.tla. The binding can fail: the order before the lock (an open with no lock), the drivers restored before the
# socket, a held lock refused at once, readers taken for a writer, and a reader let in while a writer waits (no gate)
# each take a step the spec cannot (deadlock).
foreach(seed 1 2 3 4 5)
    store_tlc_test(tlc_store_lock_trace_seed_${seed} StoreLockTrace StoreLockTrace.cfg pass
                   WALKER store_lock_walk WALK ${seed} 2000)
endforeach()
foreach(fault unlocked restore-first held-refused reader-busy no-gate)
    store_tlc_test(tlc_store_lock_trace_${fault}_rejected StoreLockTrace StoreLockTrace.cfg deadlock
                   WALKER store_lock_walk WALK 1 2000 --faulty-${fault})
endforeach()
