# Tests of nexus-server's driver supervisor (src/server/supervisor.h) and the friends layer over it, with four labels:
#   unit           supervisor_test: the state machine by rule, every refusal and every deadline at its edges, text
#                  gathering and splitting, the failed-start count, shutdown from every phase; friends_unit: the
#                  drivers file. Logic only: no processes, files, sockets or clock.
#   functional     friends_functional_*: the friends layer's real processes (the fake driver, tests/driver, and shell
#                  scripts for drivers that start processes of their own) over real pipes, on a real store in a
#                  directory of the test's own, with short deadlines.
#   model          TLC: Supervisor.tla, its reversed witnesses, and the code's walks (supervisor_walk) checked against
#                  it. Needs TLA2TOOLS_JAR and java.
#   model_nightly  TLC: Supervisor.tla on its larger instance

set(warnings ${NEXUS_WARNINGS})

add_executable(supervisor_walk supervisor_walk.cpp)
target_link_libraries(supervisor_walk PRIVATE nexus_supervisor)
target_include_directories(supervisor_walk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sync)
target_compile_options(supervisor_walk PRIVATE ${warnings})

add_executable(supervisor_test supervisor_test.cpp)
target_link_libraries(supervisor_test PRIVATE nexus_supervisor)
target_compile_options(supervisor_test PRIVATE ${warnings})

add_executable(driver_table_test driver_table_test.cpp)
target_link_libraries(driver_table_test PRIVATE nexus_supervisor)
target_compile_options(driver_table_test PRIVATE ${warnings})

# The friends layer's real processes: POSIX, like nexus-server (not built on Windows, where only the window builds).
if(NOT WIN32)
    add_executable(drivers_functional drivers_functional.cpp)
    target_link_libraries(drivers_functional PRIVATE nexus_drivers nexus_durable)
    target_compile_options(drivers_functional PRIVATE ${warnings})
    add_dependencies(drivers_functional nexus_fake_driver)
endif()

add_dependencies(model_checks supervisor_walk supervisor_test)

foreach(group start turns waiting protocol deadlines text failed_starts credential stop shutdown restart)
    add_test(NAME supervisor_unit_${group} COMMAND supervisor_test ${group})
    set_tests_properties(supervisor_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()
add_test(NAME friends_unit_driver_table COMMAND driver_table_test)
set_tests_properties(friends_unit_driver_table PROPERTIES LABELS unit TIMEOUT 30)

# Each case in a directory of its own under the build tree.
if(NOT WIN32)
    foreach(case normal stop silent exit_mid_turn malformed ignores_stop kill flood fails_first_start resume restart
                 orphan escaped_output)
        add_test(NAME friends_functional_${case} COMMAND drivers_functional $<TARGET_FILE:nexus_fake_driver>
                 ${CMAKE_CURRENT_BINARY_DIR}/friends-${case} ${case})
        set_tests_properties(friends_functional_${case} PROPERTIES LABELS functional TIMEOUT 60)
    endforeach()
endif()

# TLC (tlc_test is tests/model's): the spec on its small instance, every property.
tlc_test(tlc_supervisor MCSupervisor MCSupervisor.cfg pass WORKERS 4)
tlc_test(tlc_supervisor_wide MCSupervisor MCSupervisorWide.cfg pass WORKERS 8 NIGHTLY)

# Reversed witnesses: each broken variant must be rejected by the property named.
tlc_test(tlc_supervisor_witness_resend_after_cut WSupResendAfterCut WSupResendAfterCut.cfg invariant PROPERTY AtMostOnce)
tlc_test(tlc_supervisor_witness_turn_reuse     WSupTurnReuse     WSupTurnReuse.cfg     invariant PROPERTY TurnsIncrease)
tlc_test(tlc_supervisor_witness_stale_text     WSupStaleText     WSupStaleText.cfg     action    PROPERTY EffectsForOpen)
tlc_test(tlc_supervisor_witness_two_drivers    WSupTwoDrivers    WSupTwoDrivers.cfg    invariant PROPERTY OneDriver)
tlc_test(tlc_supervisor_witness_no_cut_on_exit WSupNoCutOnExit   WSupNoCutOnExit.cfg   invariant PROPERTY OneTurn)
tlc_test(tlc_supervisor_witness_start_loop     WSupStartLoop     WSupStartLoop.cfg     invariant PROPERTY LimitHolds)
tlc_test(tlc_supervisor_witness_no_stop_timeout WSupNoStopTimeout WSupNoStopTimeout.cfg liveness PROPERTY Settles)
tlc_test(tlc_supervisor_witness_send_while_open WSupSendWhileOpen WSupSendWhileOpen.cfg invariant PROPERTY OneTurn)
tlc_test(tlc_supervisor_witness_text_lost      WSupTextLost      WSupTextLost.cfg      action    PROPERTY TextKept)
tlc_test(tlc_supervisor_witness_start_fresh    WSupStartFresh    WSupStartFresh.cfg    action    PROPERTY StartsSound)
tlc_test(tlc_supervisor_witness_send_no_credential WSupSendNoCredential WSupSendNoCredential.cfg action
         PROPERTY NoSendWithoutCredential)

# Trace binding: the supervisor's random walks, each a behaviour of Supervisor.tla.
foreach(seed 1 2 3 4)
    tlc_test(tlc_supervisor_trace_seed_${seed} SupervisorTrace SupervisorTrace.cfg pass SUPWALK SEED ${seed} STEPS 600)
endforeach()

# The binding can fail: a restart that loses the next turn number or the session id must be rejected.
foreach(fault turn resume)
    tlc_test(tlc_supervisor_trace_faulty_${fault}_rejected SupervisorTrace SupervisorTrace.cfg invariant
             PROPERTY TraceMatches SUPWALK SEED 1 STEPS 600 FAULTY ${fault})
endforeach()
