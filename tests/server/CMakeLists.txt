# Tests of nexus-server's driver supervisor (src/server/supervisor.h) and the friends layer over it, with four labels:
#   unit           supervisor_test: the state machine by rule, every refusal and every deadline at its edges, text
#                  gathering and splitting, the failed-start count, shutdown from every phase; friends_unit: the
#                  drivers file; server_unit_home: the server's home and the settings files. Logic only: no processes,
#                  files, sockets or clock.
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

# The server's home and config, and the settings files' parsing (src/server/home.h, src/config.h).
add_executable(server_home_test home_test.cpp)
target_link_libraries(server_home_test PRIVATE nexus_supervisor)
target_compile_options(server_home_test PRIVATE ${warnings})
add_test(NAME server_unit_home COMMAND server_home_test)
set_tests_properties(server_unit_home PROPERTIES LABELS unit TIMEOUT 30)

add_executable(driver_table_test driver_table_test.cpp)
target_link_libraries(driver_table_test PRIVATE nexus_supervisor)
target_compile_options(driver_table_test PRIVATE ${warnings})

# The friends layer's real processes: POSIX, like nexus-server (not built natively on Windows, where nexus-server runs inside WSL2: CMakeLists.txt NEXUS_LINK).
if(NOT WIN32)
    add_executable(drivers_functional drivers_functional.cpp)
    target_link_libraries(drivers_functional PRIVATE nexus_drivers nexus_durable)
    target_compile_options(drivers_functional PRIVATE ${warnings})
    add_dependencies(drivers_functional nexus_fake_driver)
endif()

add_dependencies(model_checks supervisor_walk supervisor_test)

foreach(group start turns waiting protocol deadlines text failed_starts credential stop stop_hammered queue shutdown
              restart settings account)
    add_test(NAME supervisor_unit_${group} COMMAND supervisor_test ${group})
    set_tests_properties(supervisor_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()
add_test(NAME friends_unit_driver_table COMMAND driver_table_test)
set_tests_properties(friends_unit_driver_table PROPERTIES LABELS unit TIMEOUT 30)

# Each case in a directory of its own under the build tree.
if(NOT WIN32)
    foreach(case normal stop silent exit_mid_turn malformed ignores_stop kill flood fails_first_start resume restart
                 orphan escaped_output queue queue_unconfigured restart_unconfigured
                 stop_hammered account sign_in no_credential usage_limit no_account)
        add_test(NAME friends_functional_${case} COMMAND drivers_functional $<TARGET_FILE:nexus_fake_driver>
                 ${CMAKE_CURRENT_BINARY_DIR}/friends-${case} ${case})
        set_tests_properties(friends_functional_${case} PROPERTIES LABELS functional TIMEOUT 60)
    endforeach()
endif()

# TLC (tlc_test is tests/model's): the spec on its small instance, every property.
tlc_test(tlc_supervisor MCSupervisor MCSupervisor.cfg pass WORKERS 4)
tlc_test(tlc_supervisor_switch MCSupervisor MCSupervisorSwitch.cfg pass WORKERS 4)
tlc_test(tlc_supervisor_queue MCSupervisor MCSupervisorQueue.cfg pass WORKERS 4 NIGHTLY)
tlc_test(tlc_supervisor_wide MCSupervisor MCSupervisorWide.cfg pass WORKERS 8 NIGHTLY CAP 900)
tlc_test(tlc_supervisor_switch_wide MCSupervisor MCSupervisorSwitchWide.cfg pass WORKERS 8 NIGHTLY CAP 900)

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
tlc_test(tlc_supervisor_witness_take_while_open WSupTakeWhileOpen WSupTakeWhileOpen.cfg action PROPERTY TakesAtTurnEnd)
tlc_test(tlc_supervisor_witness_take_newest    WSupTakeNewest    WSupTakeNewest.cfg    action    PROPERTY TakesAtTurnEnd)
tlc_test(tlc_supervisor_witness_take_keeps     WSupTakeKeeps     WSupTakeKeeps.cfg     invariant PROPERTY QueueSound)
tlc_test(tlc_supervisor_witness_stop_again     WSupStopAgain     WSupStopAgain.cfg     invariant PROPERTY StopOnce)
tlc_test(tlc_supervisor_witness_stop_start_ignored WSupStopStartIgnored WSupStopStartIgnored.cfg action
         PROPERTY StopEndsWhatRuns)
tlc_test(tlc_supervisor_witness_stop_latched   WSupStopLatched   WSupStopLatched.cfg   action    PROPERTY NoLatchedStop)
tlc_test(tlc_supervisor_witness_setting_mid_turn WSupSettingMidTurn WSupSettingMidTurn.cfg action
         PROPERTY SettingLeavesTurn)
tlc_test(tlc_supervisor_witness_setting_ignored WSupSettingIgnored WSupSettingIgnored.cfg action
         PROPERTY WrittenWithSetting)
tlc_test(tlc_supervisor_witness_setting_not_cleared WSupSettingNotCleared WSupSettingNotCleared.cfg action
         PROPERTY WrittenWithSetting)
tlc_test(tlc_supervisor_witness_account_ignored WSupAccountIgnored WSupAccountIgnored.cfg action
         PROPERTY NoSendOnOldAccount)
tlc_test(tlc_supervisor_witness_account_cuts WSupAccountCuts WSupAccountCuts.cfg action PROPERTY AccountKeepsTurn)

# Trace binding: the supervisor's random walks, each a behaviour of Supervisor.tla.
foreach(seed 1 2 3 4 12)
    tlc_test(tlc_supervisor_trace_seed_${seed} SupervisorTrace SupervisorTrace.cfg pass SUPWALK SEED ${seed} STEPS 600)
endforeach()

# The binding can fail: a restart that loses the next turn number, the session id, the queue, the model and effort or
# the account must be rejected. The queue's seed is one whose walk restarts with prompts queued.
foreach(fault turn resume setting)
    tlc_test(tlc_supervisor_trace_faulty_${fault}_rejected SupervisorTrace SupervisorTrace.cfg invariant
             PROPERTY TraceMatches SUPWALK SEED 1 STEPS 600 FAULTY ${fault})
endforeach()
tlc_test(tlc_supervisor_trace_faulty_queue_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK SEED 12 STEPS 600 FAULTY queue)
# The account's seed is one whose walk sets an account before a restart.
tlc_test(tlc_supervisor_trace_faulty_account_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK SEED 2 STEPS 600 FAULTY account)
