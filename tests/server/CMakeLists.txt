# Tests of nexus-server's driver supervisor (src/server/supervisor.h) and the friends layer over it, with four labels:
#   unit           supervisor_test: the state machine by rule, every refusal and every deadline at its edges, text
#                  gathering and splitting, the failed-start count, shutdown from every phase; friends_unit: the
#                  drivers file; server_unit_home: the server's home and config (the settings files' own parsing is
#                  tests/window's config_unit); server_unit_pass: the
#                  order of a pass's end (flush, then release and state). Logic only: no processes,
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

# The end of a pass of the server's loop: flush, then release and state (src/server/pass.h).
add_executable(server_pass_test pass_test.cpp)
target_include_directories(server_pass_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(server_pass_test PRIVATE ${warnings})
add_test(NAME server_unit_pass COMMAND server_pass_test)
set_tests_properties(server_unit_pass PROPERTIES LABELS unit TIMEOUT 30)

add_executable(driver_table_test driver_table_test.cpp)
target_link_libraries(driver_table_test PRIVATE nexus_supervisor)
target_compile_options(driver_table_test PRIVATE ${warnings})

# The friends layer's real processes: POSIX, like nexus-server (not built natively on Windows, where nexus-server runs inside WSL2: CMakeLists.txt NEXUS_LINK).
if(NOT WIN32)
    add_executable(drivers_functional drivers_functional.cpp)
    target_link_libraries(drivers_functional PRIVATE nexus_drivers nexus_durable)
    target_compile_options(drivers_functional PRIVATE ${warnings})
    target_include_directories(drivers_functional PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    add_dependencies(drivers_functional nexus_fake_driver)
endif()

add_dependencies(model_checks supervisor_walk supervisor_test secrets_test)

foreach(group start turns waiting protocol deadlines text failed_starts credential stop stop_hammered queue shutdown
              restart settings account place limit activity signin)
    add_test(NAME supervisor_unit_${group} COMMAND supervisor_test ${group})
    set_tests_properties(supervisor_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()
add_test(NAME friends_unit_driver_table COMMAND driver_table_test)
set_tests_properties(friends_unit_driver_table PROPERTIES LABELS unit TIMEOUT 30)

# The secret dialog (docs/secrets.md): the desk, the prompt, their codecs, the hub over the server's store, the account's
# file at every crash point (all on MemFs: unit), and the leak test, which searches every output for the fake value.
# files_disk is functional: the file's and its directory's modes on the real filesystem, in a directory of its own.
# login_life is unit (a login's end, its notices, when a paste is asked for; time an argument); login_files is
# functional: a login file as it lies on the real filesystem (missing, a link, open to others, not one token, usable).
add_executable(secrets_test secrets_test.cpp)
target_link_libraries(secrets_test PRIVATE nexus_durable nexus_driver)
target_compile_options(secrets_test PRIVATE ${warnings})
target_include_directories(secrets_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
foreach(group desk prompt codecs hub files files_crash leak leak_scan login_life)
    add_test(NAME secrets_unit_${group} COMMAND secrets_test ${group})
    set_tests_properties(secrets_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()
if(NOT WIN32)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/secrets_work)
    add_test(NAME secrets_functional_files_disk COMMAND secrets_test files_disk
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/secrets_work)
    set_tests_properties(secrets_functional_files_disk PROPERTIES LABELS functional TIMEOUT 30)
    add_test(NAME secrets_functional_login_files COMMAND secrets_test login_files
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/secrets_work)
    set_tests_properties(secrets_functional_login_files PROPERTIES LABELS functional TIMEOUT 30)
endif()

# Each case in a directory of its own under the build tree.
if(NOT WIN32)
    foreach(case normal stop silent exit_mid_turn malformed ignores_stop kill flood fails_first_start resume restart
                 orphan escaped_output queue queue_unconfigured restart_unconfigured
                 stop_hammered account window unconfigured sign_in no_credential usage_limit limit_unsupported no_account instance_dir
                 instance_elsewhere instance_moved instance_default instance_removed held secret_ask secret_sign_in
                 secret_leak_witness)
        add_test(NAME friends_functional_${case} COMMAND drivers_functional $<TARGET_FILE:nexus_fake_driver>
                 ${CMAKE_CURRENT_BINARY_DIR}/friends-${case} ${case})
        set_tests_properties(friends_functional_${case} PROPERTIES LABELS functional TIMEOUT 60)
    endforeach()
endif()

# TLC (tlc_test is tests/model's): the spec on its small instance, every property.
# Per commit, each in a CI shard of its own (SHARD: capped at the shard's budget, 80 s). Alone on an idle 16-thread
# tlc host (amd-2, 2026-11-15), wall s / CPU s on eight workers and on four: plain 15.8/90 on eight; place 25.1/166 and
# 38.8/144; sign-in 16.2/100 and 29.7/108; switch 48.9/288 and 45.5/162. Switch does not get faster past four workers
# (the same 45-47 s on four CPUs as on sixteen), so it runs four and needs 44% less CPU, which is what a shard gets
# least of on a busy host; the others run eight, which nearly halves their time for 7-17% more CPU (in CI, no more
# than the runner's share of cores: tests/model/tlc-workers.cmake). Over 50 CI runs
# (pass p50/p90): plain 25/39 s, switch 46/64, place 42/55, sign-in 27/41; the timeouts were runs starved by other
# jobs on the host (sign-in generated 3,633 of its states in 55 s), not instances grown past their cap.
tlc_test(tlc_supervisor MCSupervisor MCSupervisor.cfg pass WORKERS 8 SHARD)
tlc_test(tlc_supervisor_switch MCSupervisor MCSupervisorSwitch.cfg pass WORKERS 4 SHARD)
tlc_test(tlc_supervisor_place MCSupervisor MCSupervisorPlace.cfg pass WORKERS 8 SHARD)
tlc_test(tlc_supervisor_signin MCSupervisor MCSupervisorSignIn.cfg pass WORKERS 8 SHARD)
# The limit's per-commit instance runs in the supervisor-rest shard on two workers, like the witnesses beside it:
# 4,497 states, 4 s on an idle machine, so the 60 s cap leaves fifteen times that for a loaded runner. Its two-prompt
# instance (69 s on two workers) and the wide one run nightly.
tlc_test(tlc_supervisor_limit MCSupervisor MCSupervisorLimit.cfg pass)
tlc_test(tlc_supervisor_limit_two MCSupervisor MCSupervisorLimitTwo.cfg pass WORKERS 8 NIGHTLY CAP 900)
tlc_test(tlc_supervisor_limit_wide MCSupervisor MCSupervisorLimitWide.cfg pass WORKERS 8 NIGHTLY CAP 900)
tlc_test(tlc_supervisor_queue MCSupervisor MCSupervisorQueue.cfg pass WORKERS 4 NIGHTLY)
tlc_test(tlc_supervisor_wide MCSupervisor MCSupervisorWide.cfg pass WORKERS 8 NIGHTLY CAP 900)
tlc_test(tlc_supervisor_switch_wide MCSupervisor MCSupervisorSwitchWide.cfg pass WORKERS 8 NIGHTLY CAP 900)
tlc_test(tlc_supervisor_place_wide MCSupervisor MCSupervisorPlaceWide.cfg pass WORKERS 8 NIGHTLY CAP 900)

# Reversed witnesses: each broken variant must be rejected by the property named.
tlc_test(tlc_supervisor_witness_resend_after_cut WSupResendAfterCut WSupResendAfterCut.cfg invariant PROPERTY AtMostOnce)
tlc_test(tlc_supervisor_witness_turn_reuse     WSupTurnReuse     WSupTurnReuse.cfg     invariant PROPERTY TurnsIncrease)
tlc_test(tlc_supervisor_witness_stale_text     WSupStaleText     WSupStaleText.cfg     action    PROPERTY EffectsForOpen)
tlc_test(tlc_supervisor_witness_two_drivers    WSupTwoDrivers    WSupTwoDrivers.cfg    invariant PROPERTY OneDriver)
tlc_test(tlc_supervisor_witness_no_cut_on_exit WSupNoCutOnExit   WSupNoCutOnExit.cfg   invariant PROPERTY OneTurn)
tlc_test(tlc_supervisor_witness_start_loop     WSupStartLoop     WSupStartLoop.cfg     invariant PROPERTY LimitHolds)
tlc_test(tlc_supervisor_witness_no_stop_timeout WSupNoStopTimeout WSupNoStopTimeout.cfg liveness PROPERTY Settles)
tlc_test(tlc_supervisor_witness_send_while_open WSupSendWhileOpen WSupSendWhileOpen.cfg invariant PROPERTY OneTurn)
tlc_test(tlc_supervisor_witness_write_before_flush WSupWriteBeforeFlush WSupWriteBeforeFlush.cfg invariant
    PROPERTY AtMostOnce)
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
tlc_test(tlc_supervisor_witness_place_ignored WSupPlaceIgnored WSupPlaceIgnored.cfg action PROPERTY NoSendInOldPlace)
tlc_test(tlc_supervisor_witness_resume_elsewhere WSupResumeElsewhere WSupResumeElsewhere.cfg action
         PROPERTY ResumesItsDir)
tlc_test(tlc_supervisor_witness_summary_lost WSupSummaryLost WSupSummaryLost.cfg action PROPERTY SummaryOwed)
# The limit: a driver started without it is never kept for a limited session, and tightening stops an open turn.
tlc_test(tlc_supervisor_witness_limit_not_retired WSupLimitNotRetired WSupLimitNotRetired.cfg invariant
         PROPERTY NeverWeaker)
tlc_test(tlc_supervisor_witness_limit_no_stop WSupLimitNoStop WSupLimitNoStop.cfg invariant PROPERTY NeverWeaker)
# The state cursor's activity (ActivityOf): idle while a prompt waits, a crash shown as idle, and down that turns idle
# when its driver is collected.
tlc_test(tlc_supervisor_witness_idle_while_waiting WSupIdleWhileWaiting WSupIdleWhileWaiting.cfg invariant
         PROPERTY BrokenIdleWaitsForPerson)
tlc_test(tlc_supervisor_witness_crash_idle WSupCrashIdle WSupCrashIdle.cfg action PROPERTY BrokenCrashShowsDown)
tlc_test(tlc_supervisor_witness_down_forgotten WSupDownForgotten WSupDownForgotten.cfg action
         PROPERTY BrokenDownLeavesByWork)
# A friend with no driver configured shown idle.
tlc_test(tlc_supervisor_witness_no_driver_idle WSupNoDriverIdle WSupNoDriverIdle.cfg invariant
         PROPERTY BrokenNoDriverShowsDown)

# Trace binding: the supervisor's random walks, each a behaviour of Supervisor.tla.
foreach(seed 1 2 3 4 12)
    tlc_test(tlc_supervisor_trace_seed_${seed} SupervisorTrace SupervisorTrace.cfg pass SUPWALK SEED ${seed} STEPS 600)
endforeach()

# The binding can fail: a restart that loses the next turn number, the session id, the queue, the model and effort, the
# account or the harness session's directory must be rejected. The queue's seed is one whose walk restarts with
# prompts queued.
foreach(fault turn resume)
    tlc_test(tlc_supervisor_trace_faulty_${fault}_rejected SupervisorTrace SupervisorTrace.cfg invariant
             PROPERTY TraceMatches SUPWALK SEED 1 STEPS 600 FAULTY ${fault})
endforeach()
# The setting's seed is one whose walk sets a model or effort before a restart.
tlc_test(tlc_supervisor_trace_faulty_setting_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK SEED 1 STEPS 600 FAULTY setting)
tlc_test(tlc_supervisor_trace_faulty_queue_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK SEED 12 STEPS 600 FAULTY queue)
# The account's seed is one whose walk sets an account before a restart.
tlc_test(tlc_supervisor_trace_faulty_account_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK SEED 2 STEPS 600 FAULTY account)
# The directory's seed is one whose walk restarts with a harness session made in another directory than the session's.
tlc_test(tlc_supervisor_trace_faulty_dir_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK SEED 2 STEPS 600 FAULTY dir)
# The limit's seed is one whose walk restarts while the session is limited.
tlc_test(tlc_supervisor_trace_faulty_limit_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK SEED 1 STEPS 600 FAULTY limit)
# The code's activity (the window's state cursor) is bound too: a walk whose recorded activity reads a crash as idle is
# rejected.
tlc_test(tlc_supervisor_trace_faulty_activity_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK SEED 1 STEPS 600 FAULTY activity)
