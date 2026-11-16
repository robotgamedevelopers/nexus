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

# One pass of the server's loop (src/server/pass.h): unit test and trace walk (models/LoopPass.tla).
add_executable(server_pass_test pass_test.cpp)
target_include_directories(server_pass_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(server_pass_test PRIVATE ${warnings})
add_test(NAME server_unit_pass COMMAND server_pass_test)
set_tests_properties(server_unit_pass PROPERTIES LABELS unit TIMEOUT 30)

add_executable(pass_walk pass_walk.cpp)
target_link_libraries(pass_walk PRIVATE nexus_drivers nexus_durable)
target_include_directories(pass_walk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sync)
target_compile_options(pass_walk PRIVATE ${warnings})

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

add_dependencies(model_checks supervisor_walk supervisor_test secrets_test signin_test server_pass_test pass_walk)

foreach(group start turns waiting protocol deadlines text failed_starts credential stop stop_hammered friend_removed
              hub_friend_removed queue shutdown restart settings account place limit activity signin)
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

# In-app sign-in (docs/secrets.md "Sign-in"): the URL and callback parsing, the desk, the window's opener, the one-shot
# gate's logic, the token scanner, the codecs, the hub over the server's store with the runner's hooks recorded, and the
# leak test over every output. All unit: no time, no sockets, no processes, no files (MemFs).
add_executable(signin_test signin_test.cpp)
target_link_libraries(signin_test PRIVATE nexus_durable)
target_compile_options(signin_test PRIVATE ${warnings})
target_include_directories(signin_test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
foreach(group urls desk opener gate scanner codecs hub hub_late hub_older leak leak_scan)
    add_test(NAME signin_unit_${group} COMMAND signin_test ${group})
    set_tests_properties(signin_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()

# Sign-in's processes for real, functional: the runner starting a FAKE `claude` (fake_claude.cpp: a made-up URL, a
# listener on loopback, a made-up token) under a pty, the helper, the window's gate on loopback, the replay; the test
# plays the browser. Each case in a directory of its own under the build tree. POSIX, as the runner is.
if(NOT WIN32)
    add_executable(nexus_fake_claude fake_claude.cpp)
    target_compile_options(nexus_fake_claude PRIVATE ${warnings})
    add_executable(signin_functional signin_functional.cpp)
    target_link_libraries(signin_functional PRIVATE nexus_signin_runner nexus_signin_gate)
    target_compile_options(signin_functional PRIVATE ${warnings})
    target_include_directories(signin_functional PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    add_dependencies(signin_functional nexus_fake_claude nexus-signin-open)
    foreach(case same_machine printed_url forwarded declined api_key stopped unguarded gate_in_use gate_v6_in_use late crashed
                 forwarded_v6 quick_exit group)
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/signin-${case})
        add_test(NAME signin_functional_${case}
                 COMMAND signin_functional $<TARGET_FILE:nexus_fake_claude> $<TARGET_FILE:nexus-signin-open> ${case}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/signin-${case})
        set_tests_properties(signin_functional_${case} PROPERTIES LABELS functional TIMEOUT 60)
    endforeach()
endif()

# Each case in a directory of its own under the build tree.
if(NOT WIN32)
    foreach(case normal stop silent exit_mid_turn malformed ignores_stop kill flood fails_first_start resume restart
                 orphan escaped_output queue queue_unconfigured restart_unconfigured
                 stop_hammered account window unconfigured sign_in no_credential usage_limit limit_unsupported no_account
                 friend_removed friend_back_limited friend_removed_starting friend_back_before_stop
                 friend_back_limited_queue friend_back_moved friend_back_account no_registry no_registry_listed
                 instance_dir instance_elsewhere instance_moved
                 instance_default instance_removed held secret_ask secret_sign_in secret_leak_witness)
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
# Each is split (SPLIT): Settles, the one temporal property, is where the time goes (switch on four cores of a loaded
# host: every invariant and the 20 action properties 7.8 s, Settles alone 67.9 s, all together 122 s), and checked by
# halves it costs less than half (Settles of one prompt 38.7 s, of both 94.2 s). So <name>_temporal_1 and _2 check
# SettlesOdd and SettlesEven (Supervisor.tla: together, Settles) in shards of their own, and <name>_rest every
# invariant and the other properties, a few seconds each, in the supervisor-rest shard. Between them every property is
# checked on the same instance.
tlc_test(tlc_supervisor MCSupervisor MCSupervisor.cfg pass WORKERS 8 SHARD SPLIT Settles=SettlesOdd+SettlesEven)
tlc_test(tlc_supervisor_switch MCSupervisor MCSupervisorSwitch.cfg pass WORKERS 4 SHARD
    SPLIT Settles=SettlesOdd+SettlesEven)
tlc_test(tlc_supervisor_place MCSupervisor MCSupervisorPlace.cfg pass WORKERS 8 SHARD
    SPLIT Settles=SettlesOdd+SettlesEven)
tlc_test(tlc_supervisor_signin MCSupervisor MCSupervisorSignIn.cfg pass WORKERS 8 SHARD
    SPLIT Settles=SettlesOdd+SettlesEven)
# The limit's per-commit instance runs in the supervisor-rest shard on two workers, like the witnesses beside it:
# 4,497 states, 4 s on an idle machine, so the 60 s cap leaves fifteen times that for a loaded runner. Its two-prompt
# instance (69 s on two workers) and the wide one run nightly.
tlc_test(tlc_supervisor_limit MCSupervisor MCSupervisorLimit.cfg pass)
tlc_test(tlc_supervisor_limit_two MCSupervisor MCSupervisorLimitTwo.cfg pass WORKERS 8 NIGHTLY CAP 900)
tlc_test(tlc_supervisor_limit_wide MCSupervisor MCSupervisorLimitWide.cfg pass WORKERS 8 NIGHTLY CAP 900)
# The limit with the friend taken out of friends.json and put back (MCSupervisorLimitFriend.cfg: 15,450 states, 40 s on
# two workers), nightly; per commit, the walk's limit changes meet its removals in tlc_supervisor_trace_friends_*.
tlc_test(tlc_supervisor_limit_friend MCSupervisor MCSupervisorLimitFriend.cfg pass WORKERS 8 NIGHTLY CAP 900)
# The friend taken out of friends.json and put back: per commit with no server restart (MCSupervisorFriend.cfg, split
# as the instances above, its Settles halves each in a CI shard of their own), and nightly with a restart while it is out as well (MCSupervisorFriendRestart.cfg,
# 65,189 states, 62 s on eight workers on an M-series Mac, most of it the liveness check). The walk's restarts while
# the friend is out are checked per commit (tlc_supervisor_trace_friends_*).
tlc_test(tlc_supervisor_friend MCSupervisor MCSupervisorFriend.cfg pass WORKERS 8 SHARD
    SPLIT Settles=SettlesOdd+SettlesEven)
tlc_test(tlc_supervisor_friend_restart MCSupervisor MCSupervisorFriendRestart.cfg pass WORKERS 8 NIGHTLY CAP 900)
# No friends list held (a start whose committed files do not read), then the first list applied: per commit, in
# supervisor-rest (5,571 states, 6 s on eight workers).
tlc_test(tlc_supervisor_friend_unknown MCSupervisor MCSupervisorFriendUnknown.cfg pass WORKERS 8)
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
# The friend taken out of friends.json: a removal that leaves the turn running, and a take while it is out.
tlc_test(tlc_supervisor_witness_remove_leaves_turn WSupRemoveLeavesTurn WSupRemoveLeavesTurn.cfg invariant
         PROPERTY AbsentRunsNothing)
tlc_test(tlc_supervisor_witness_take_while_absent WSupTakeWhileAbsent WSupTakeWhileAbsent.cfg action
         PROPERTY AbsentKeepsPrompts)
# A start holding no friends list that reads the friend as in it.
tlc_test(tlc_supervisor_witness_unknown_listed WSupUnknownListed WSupUnknownListed.cfg action PROPERTY NoSendUnknown)

# Trace binding: the supervisor's random walks, each a behaviour of Supervisor.tla.
foreach(seed 1 2 3 4 12)
    tlc_test(tlc_supervisor_trace_seed_${seed} SupervisorTrace SupervisorTrace.cfg pass SUPWALK SEED ${seed} STEPS 600)
endforeach()
# With the friend taken out of friends.json and put back among the other steps. Seed 13 restarts once holding no
# friends list, then applies one with the friend in it; seed 15 also applies one without it, and restarts again after.
foreach(seed 13 14 15)
    tlc_test(tlc_supervisor_trace_friends_seed_${seed} SupervisorTrace SupervisorTrace.cfg pass SUPWALK CHURN
             SEED ${seed} STEPS 600)
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
# A server whose supervisor is never told the friend was taken out of friends.json is rejected.
tlc_test(tlc_supervisor_trace_faulty_remove_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK CHURN SEED 13 STEPS 600 FAULTY remove)
# A restart holding no friends list that starts the supervisor with its friend in it is rejected.
tlc_test(tlc_supervisor_trace_faulty_unknown_rejected SupervisorTrace SupervisorTrace.cfg invariant
         PROPERTY TraceMatches SUPWALK CHURN SEED 13 STEPS 600 FAULTY unknown)

# Placement (src/server/builds.h, docs/builds.md, models/placement):
#   unit        builds_test (builds_unit_*): the manifest, the hash, the evidence, the formats rule, the names in the
#               builds directory, prune's choice, the program's build. Text and decisions only: no files, clock or
#               sockets.
#   functional  builds_functional (builds_functional_*): the verbs on real files in a directory of the test's own (a
#               scratch HOME of its own; never ~/.nexus): a crash (a child process killed) at every step of place and of
#               current, then a fresh run; two placements and two switches at once, many rounds; untested, a tampered
#               file, a stale mark, a driver changed after its tests, all refused; prune with a build in use; a full
#               disk; a filesystem whose locks are not kept.
#   model       TLC: Placement.tla, its reversed witnesses, and the code's walks (builds_walk, functional: real files,
#               threads) checked against it; the faulty walks rejected.
#   model_nightly  Placement.tla on its larger instances (two actors two verbs each; one actor three verbs).
add_executable(builds_test builds_test.cpp)
target_link_libraries(builds_test PRIVATE nexus_builds)
target_compile_options(builds_test PRIVATE ${warnings})
foreach(group sha manifest evidence formats names prune program)
    add_test(NAME builds_unit_${group} COMMAND builds_test ${group})
    set_tests_properties(builds_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()

if(NOT WIN32)
    add_executable(builds_functional builds_functional.cpp)
    target_link_libraries(builds_functional PRIVATE nexus_builds)
    target_compile_options(builds_functional PRIVATE ${warnings})
    foreach(case place_and_switch crash_place crash_switch crash_prune two_places two_switches refusals
                 prune_in_use disk_full unkept_locks hold help)
        add_test(NAME builds_functional_${case} COMMAND builds_functional ${case}
                 ${CMAKE_CURRENT_BINARY_DIR}/builds-${case})
        set_tests_properties(builds_functional_${case} PROPERTIES LABELS functional TIMEOUT 120)
    endforeach()

    find_package(Threads REQUIRED)
    add_executable(builds_walk builds_walk.cpp)
    target_link_libraries(builds_walk PRIVATE nexus_builds Threads::Threads)
    target_compile_options(builds_walk PRIVATE ${warnings})
    add_dependencies(model_checks builds_walk builds_test)

    # TLC through the store's runner (tests/store/tlc.cmake) on models/placement.
    set(placement_models "${PROJECT_SOURCE_DIR}/models/placement")
    set(placement_tlc "${PROJECT_SOURCE_DIR}/tests/store/tlc.cmake")
    function(placement_tlc_test name cfg module expect)
        cmake_parse_arguments(T "NIGHTLY" "PROPERTY;WORKERS;CAP" "WALK" ${ARGN})
        if(NOT T_WORKERS)
            set(T_WORKERS 2)
        endif()
        set(label model)
        if(NOT T_CAP)
            set(T_CAP 60)
        endif()
        if(T_NIGHTLY)
            set(label model_nightly)
        endif()
        math(EXPR ctest_timeout "${T_CAP} + 30")
        set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${placement_models} -DMODULE=${module}
                 -DCFG=${cfg} -DEXPECT=${expect} -DWORKERS=${T_WORKERS} -DCAP=${T_CAP})
        if(T_PROPERTY)
            list(APPEND defs -DPROPERTY=${T_PROPERTY})
        endif()
        if(T_WALK)
            string(REPLACE ";" "," walk_args "${T_WALK}")
            list(APPEND defs -DWALK=$<TARGET_FILE:builds_walk> -DWALK_ARGS=${walk_args}
                 -DWALK_OUT=PlacementTraceData.tla)
        endif()
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${placement_tlc})
        set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT ${ctest_timeout} PROCESSORS ${T_WORKERS})
    endfunction()

    # The spec on its small instances, every invariant, and VerbsEnd under fairness.
    # spec: 306,768 states, 50 s on 4 workers, a shard of its own (ci.yml, placement-spec), capped at the shards' 80 s;
    # seq: 73,043 states, 9 s on 4 workers.
    placement_tlc_test(tlc_placement_spec Placement.cfg Placement pass WORKERS 4 CAP 80)
    placement_tlc_test(tlc_placement_seq PlacementSeq.cfg Placement pass WORKERS 4)
    placement_tlc_test(tlc_placement_large PlacementLarge.cfg Placement pass WORKERS 8 NIGHTLY CAP 600)
    placement_tlc_test(tlc_placement_seq_three PlacementSeqThree.cfg Placement pass WORKERS 8 NIGHTLY CAP 600)

    # Reversed witnesses: each broken variant must be rejected by the invariant named.
    placement_tlc_test(tlc_placement_witness_two_renames witnesses/PTwoRenames.cfg Placement invariant
                       PROPERTY PrevIsLastCurrent)
    placement_tlc_test(tlc_placement_witness_no_lock witnesses/PNoLock.cfg Placement invariant
                       PROPERTY PrevIsLastCurrent)
    placement_tlc_test(tlc_placement_witness_in_place witnesses/PInPlace.cfg Placement invariant
                       PROPERTY CurrentValid)
    placement_tlc_test(tlc_placement_witness_tested_program_only witnesses/PTestedProgramOnly.cfg Placement invariant
                       PROPERTY CurrentValid)
    placement_tlc_test(tlc_placement_witness_stale_marker witnesses/PStaleMarker.cfg Placement invariant
                       PROPERTY CurrentValid)
    placement_tlc_test(tlc_placement_witness_prune_in_use witnesses/PPruneInUse.cfg Placement invariant
                       PROPERTY InUseKept)
    placement_tlc_test(tlc_placement_witness_reuse_tmp witnesses/PReuseTmp.cfg Placement invariant
                       PROPERTY NamedWhole)
    placement_tlc_test(tlc_placement_witness_no_sync witnesses/PNoSync.cfg Placement invariant
                       PROPERTY NamedWhole)

    # Trace binding: the real code's walks on real files, each a behaviour of Placement.tla: a behaviour reaches the
    # trace's last record, which TLC reports as TraceUnfinished violated (PlacementTrace.tla says why).
    foreach(seed 1 2 3)
        placement_tlc_test(tlc_placement_trace_seed_${seed} PlacementTrace.cfg PlacementTrace invariant
                           PROPERTY TraceUnfinished WALK ${seed} 800)
    endforeach()
    # The binding can fail: each reversed fault's walk is not a behaviour of the spec. No behaviour reaches its end, so
    # TLC explores them all and ends with no error: `pass` here is the rejection. Each seed is one whose walk reaches
    # the fault (the trace is matched up to the step the fault changes, and no further): prune-in-use's needs a server
    # up on a build a prune would remove.
    foreach(fault_seed no-lock:1 no-sync:1 tested-program-only:1 reuse-tmp:1 prune-in-use:6 two-renames:1)
        string(REPLACE ":" ";" fs "${fault_seed}")
        list(GET fs 0 fault)
        list(GET fs 1 seed)
        placement_tlc_test(tlc_placement_trace_${fault}_rejected PlacementTrace.cfg PlacementTrace pass
                           WALK ${seed} 800 --fault ${fault})
    endforeach()
endif()

# One pass of nexus-server's loop (models/LoopPass.tla): tick, hold, apply, flush, release, state and crashes.
# Per commit, MCLoopPass.cfg (22,500 distinct states, with Converges) runs in loop-pass-spec (SHARD: 80 s cap, no
# walk build), and MCLoopPassTwo.cfg (58,851 distinct states), all eleven reversed witnesses, five seeded code walks
# and five faulty walks run in loop-pass-rest. Nightly, MCLoopPassWide.cfg (113,337 distinct states) and
# MCLoopPassThree.cfg (681,678 distinct states).
tlc_test(tlc_loop_pass_spec MCLoopPass MCLoopPass.cfg pass WORKERS 4 SHARD)
tlc_test(tlc_loop_pass_two  MCLoopPass MCLoopPassTwo.cfg pass WORKERS 4)
tlc_test(tlc_loop_pass_wide  MCLoopPass MCLoopPassWide.cfg pass WORKERS 8 NIGHTLY)
tlc_test(tlc_loop_pass_three MCLoopPass MCLoopPassThree.cfg pass WORKERS 8 NIGHTLY)

tlc_test(tlc_loop_pass_witness_release_before_flush WLoopReleaseBeforeFlush WLoopReleaseBeforeFlush.cfg invariant
         PROPERTY PromptAtMostOnce)
tlc_test(tlc_loop_pass_witness_state_before_flush   WLoopStateBeforeFlush   WLoopStateBeforeFlush.cfg   invariant
         PROPERTY LinkStateDurable)
tlc_test(tlc_loop_pass_witness_tick_before_flush    WLoopTickBeforeFlush    WLoopTickBeforeFlush.cfg    invariant
         PROPERTY AckedOnlyDurable)
tlc_test(tlc_loop_pass_witness_no_hold              WLoopNoHold              WLoopNoHold.cfg              invariant
         PROPERTY PromptAtMostOnce)
tlc_test(tlc_loop_pass_witness_release_on_fail      WLoopReleaseOnFail      WLoopReleaseOnFail.cfg      invariant
         PROPERTY DriverOnlyDurable)
tlc_test(tlc_loop_pass_witness_tick_on_fail         WLoopTickOnFail         WLoopTickOnFail.cfg         invariant
         PROPERTY AckedOnlyDurable)
tlc_test(tlc_loop_pass_witness_reset_applier        WLoopResetApplier        WLoopResetApplier.cfg        invariant
         PROPERTY ExactlyOnce)
tlc_test(tlc_loop_pass_witness_no_tick              WLoopNoTick              WLoopNoTick.cfg              liveness
         PROPERTY Converges)
tlc_test(tlc_loop_pass_witness_turn_reuse           WLoopTurnReuse           WLoopTurnReuse.cfg           invariant
         PROPERTY TurnsIncrease)
tlc_test(tlc_loop_pass_witness_crash_drops_durable  WLoopCrashDropsDurable  WLoopCrashDropsDurable.cfg  action
         PROPERTY DurableMonotonic)
tlc_test(tlc_loop_pass_witness_out_before_flush     WLoopOutBeforeFlush     WLoopOutBeforeFlush.cfg     action
         PROPERTY NoOutBeforeFlush)

foreach(seed 1 2 3 4 5)
    tlc_test(tlc_loop_pass_trace_seed_${seed} LoopPassTrace LoopPassTrace.cfg pass PASSWALK SEED ${seed} STEPS 250)
endforeach()
foreach(fault release-before-flush state-before-flush tick-before-flush no-hold release-on-fail)
    tlc_test(tlc_loop_pass_trace_faulty_${fault}_rejected LoopPassTrace LoopPassTrace.cfg deadlock
             PASSWALK SEED 1 STEPS 80 FAULTY ${fault})
endforeach()

# Handover (src/server/handover.h, src/server/drain.h, docs/builds.md, models/handover):
#   unit        handover_test (handover_unit_*): Drain, status second-line formatting/parsing, StateRecord codec,
#               handover/drain/legacy/cold-start over MemFs, broken-build rollback, cut-and-resume at every hook step
#               and every MemFs::fail_at + crash point, and refusals. No real files, processes, sockets or clock.
#   functional  handover_functional (handover_functional_*): two-build drain (idle, open turn with durable store commit,
#               drain deadline, pre-drain legacy stop), broken-build rollback (crash and hung store-lock holder),
#               fork/_exit crash at every hook step followed by resume, concurrent handovers, and unlockable filesystem
#               refusals.
#   model       TLC: Handover.tla (spec, cut-and-resume, legacy), 11 reversed witnesses, and handover_walk traces
#               checked against HandoverTrace.tla (plus 8 faulty walks rejected).
#   model_nightly  Handover.tla on its larger two-actor, two-command, two-crash instance.
add_executable(handover_test handover_test.cpp)
target_link_libraries(handover_test PRIVATE nexus_handover)
target_compile_options(handover_test PRIVATE ${warnings})
foreach(group drain state handover rollback crash refusals)
    add_test(NAME handover_unit_${group} COMMAND handover_test ${group})
    set_tests_properties(handover_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()

if(NOT WIN32)
    add_executable(nexus_fake_build_server fake_build_server.cpp)
    target_link_libraries(nexus_fake_build_server PRIVATE nexus_supervision nexus_handover nexus_builds nexus_store)
    target_compile_options(nexus_fake_build_server PRIVATE ${warnings})

    add_executable(handover_functional handover_functional.cpp)
    target_link_libraries(handover_functional PRIVATE nexus_handover nexus_builds nexus_store)
    target_compile_options(handover_functional PRIVATE ${warnings})
    add_dependencies(handover_functional nexus_fake_build_server)

    string(MD5 handover_build_hash "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${handover_build_hash}" 0 10 handover_build_hash)
    set(handover_func_base "/tmp/nexus-ho-${handover_build_hash}")
    foreach(case drain_idle_and_turn drain_deadline_and_legacy rollback_crash_and_hang crash_every_step
                 concurrent_and_locks)
        add_test(NAME handover_functional_${case} COMMAND handover_functional
                 $<TARGET_FILE:nexus_fake_build_server> ${case} ${handover_func_base}-${case})
        set_tests_properties(handover_functional_${case} PROPERTIES LABELS functional TIMEOUT 120)
    endforeach()

    add_executable(handover_walk handover_walk.cpp)
    target_link_libraries(handover_walk PRIVATE nexus_handover nexus_builds nexus_store Threads::Threads)
    target_compile_options(handover_walk PRIVATE ${warnings})
    add_dependencies(model_checks handover_walk handover_test)

    set(handover_models "${PROJECT_SOURCE_DIR}/models/handover")
    function(handover_tlc_test name cfg module expect)
        cmake_parse_arguments(T "NIGHTLY" "PROPERTY;WORKERS;CAP" "WALK" ${ARGN})
        if(NOT T_WORKERS)
            set(T_WORKERS 2)
        endif()
        set(label model)
        if(NOT T_CAP)
            set(T_CAP 60)
        endif()
        if(T_NIGHTLY)
            set(label model_nightly)
        endif()
        math(EXPR ctest_timeout "${T_CAP} + 30")
        set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${handover_models} -DMODULE=${module}
                 -DCFG=${cfg} -DEXPECT=${expect} -DWORKERS=${T_WORKERS} -DCAP=${T_CAP})
        if(T_PROPERTY)
            list(APPEND defs -DPROPERTY=${T_PROPERTY})
        endif()
        if(T_WALK)
            string(REPLACE ";" "," walk_args "${T_WALK}")
            list(APPEND defs -DWALK=$<TARGET_FILE:handover_walk> -DWALK_ARGS=${walk_args}
                 -DWALK_OUT=HandoverTraceData.tla)
        endif()
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${placement_tlc})
        set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT ${ctest_timeout} PROCESSORS ${T_WORKERS})
    endfunction()

    handover_tlc_test(tlc_handover_spec Handover.cfg Handover pass WORKERS 4 CAP 80)
    handover_tlc_test(tlc_handover_cut_resume HandoverCutResume.cfg Handover pass WORKERS 4 CAP 80)
    handover_tlc_test(tlc_handover_legacy HandoverLegacy.cfg Handover pass WORKERS 4 CAP 80)
    handover_tlc_test(tlc_handover_large HandoverLarge.cfg Handover pass WORKERS 8 NIGHTLY CAP 600)

    handover_tlc_test(tlc_handover_witness_no_lock witnesses/HNoLock.cfg Handover invariant
                      PROPERTY OneHandover)
    handover_tlc_test(tlc_handover_witness_no_sync witnesses/HNoSync.cfg Handover invariant
                      PROPERTY IdleCurrentWorks)
    handover_tlc_test(tlc_handover_witness_in_memory_only witnesses/HInMemoryOnly.cfg Handover invariant
                      PROPERTY IdleCurrentWorks)
    handover_tlc_test(tlc_handover_witness_overwrite_prev witnesses/HOverwritePrev.cfg Handover invariant
                      PROPERTY StateValid)
    handover_tlc_test(tlc_handover_witness_no_rollback witnesses/HNoRollback.cfg Handover invariant
                      PROPERTY IdleCurrentWorks)
    handover_tlc_test(tlc_handover_witness_stop_cuts witnesses/HStopCutsTurn.cfg Handover action
                      PROPERTY DrainCutsOnlyLate)
    handover_tlc_test(tlc_handover_witness_stop_idle witnesses/HStopIdleNonLegacy.cfg Handover action
                      PROPERTY StopOnlyLegacy)
    handover_tlc_test(tlc_handover_witness_no_drain_deadline witnesses/HNoDrainDeadline.cfg Handover temporal
                      PROPERTY HandoverEnds)
    handover_tlc_test(tlc_handover_witness_untested_target witnesses/HUntestedTarget.cfg Handover invariant
                      PROPERTY CurrentTested)
    handover_tlc_test(tlc_handover_witness_serve_broken witnesses/HServeBroken.cfg Handover invariant
                      PROPERTY NeverServesBroken)
    handover_tlc_test(tlc_handover_witness_resume_ignores_state witnesses/HResumeIgnoresState.cfg Handover temporal
                      PROPERTY AlwaysRecoversToServing)

    foreach(seed 1 2 3)
        handover_tlc_test(tlc_handover_trace_seed_${seed} HandoverTrace.cfg HandoverTrace invariant
                          PROPERTY TraceUnfinished WALK ${seed} 600)
    endforeach()
    foreach(fault_seed no-lock:1 no-sync:1 in-memory-only:1 overwrite-prev:2 no-rollback:1 stop-cuts:1
                       allow-untested:1 serve-broken:1)
        string(REPLACE ":" ";" fs "${fault_seed}")
        list(GET fs 0 fault)
        list(GET fs 1 seed)
        handover_tlc_test(tlc_handover_trace_${fault}_rejected HandoverTrace.cfg HandoverTrace pass
                          WALK ${seed} 600 --fault ${fault})
    endforeach()
endif()

# Supervision (src/server/supervision.h, src/server/supervise_state.h, docs/builds.md, models/supervision):
#   unit        supervision_test (supervision_unit_*): SuperviseState codec and atomic updates, deterministic systemd
#               unit and launchd plist generation and escaping, cold start / drain-restart (75) / stop-stays-stopped (0)
#               / --start, post-start failure rollback with stability window, dual-broken shunning and halt bound (78),
#               and MemFs fail_at + crash recovery sweeps. No real files, processes, sockets or sleeps.
#   functional  supervision_functional (supervision_functional_*): real supervise child processes over real sockets and
#               OS locks in isolated temp dirs (start/stop/--start/SIGTERM, drain handover + adopted crash recovery,
#               post-SERVING crash triggering automatic rollback to previous, dual-broken halt without ping-pong,
#               offline unit generation/verification, busy lock, and unlockable filesystem refusals).
#   model       TLC: Supervision.tla (spec, post-start rollback, dual-fail), 9 reversed witnesses, and supervision_walk
#               traces checked against SupervisionTrace.tla (plus 9 faulty walks rejected).
#   model_nightly  Supervision.tla on its larger 3-build, 2-handover, 2-supervisor-death instance.
add_executable(supervision_test supervision_test.cpp)
target_link_libraries(supervision_test PRIVATE nexus_supervision)
target_compile_options(supervision_test PRIVATE ${warnings})
foreach(group state units lifecycle rollback dual_fail crash)
    add_test(NAME supervision_unit_${group} COMMAND supervision_test ${group})
    set_tests_properties(supervision_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()

if(NOT WIN32)
    add_executable(supervision_functional supervision_functional.cpp)
    target_link_libraries(supervision_functional PRIVATE nexus_supervision nexus_handover nexus_builds nexus_store)
    target_compile_options(supervision_functional PRIVATE ${warnings})
    add_dependencies(supervision_functional nexus_fake_build_server)

    set(supervision_func_base "/tmp/nexus-sup-${handover_build_hash}")
    foreach(case start_and_stop drain_handover post_start_rollback dual_fail_halt units_and_locks)
        add_test(NAME supervision_functional_${case} COMMAND supervision_functional
                 $<TARGET_FILE:nexus_fake_build_server> ${case} ${supervision_func_base}-${case})
        set_tests_properties(supervision_functional_${case} PROPERTIES LABELS functional TIMEOUT 120)
    endforeach()

    add_executable(supervision_walk supervision_walk.cpp)
    target_link_libraries(supervision_walk PRIVATE nexus_supervision nexus_handover nexus_builds nexus_store
                          Threads::Threads)
    target_compile_options(supervision_walk PRIVATE ${warnings})
    add_dependencies(model_checks supervision_walk supervision_test)

    set(supervision_models "${PROJECT_SOURCE_DIR}/models/supervision")
    function(supervision_tlc_test name cfg module expect)
        cmake_parse_arguments(T "NIGHTLY" "PROPERTY;WORKERS;CAP" "WALK" ${ARGN})
        if(NOT T_WORKERS)
            set(T_WORKERS 2)
        endif()
        set(label model)
        if(NOT T_CAP)
            set(T_CAP 60)
        endif()
        if(T_NIGHTLY)
            set(label model_nightly)
        endif()
        math(EXPR ctest_timeout "${T_CAP} + 30")
        set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${supervision_models} -DMODULE=${module}
                 -DCFG=${cfg} -DEXPECT=${expect} -DWORKERS=${T_WORKERS} -DCAP=${T_CAP})
        if(T_PROPERTY)
            list(APPEND defs -DPROPERTY=${T_PROPERTY})
        endif()
        if(T_WALK)
            string(REPLACE ";" "," walk_args "${T_WALK}")
            list(APPEND defs -DWALK=$<TARGET_FILE:supervision_walk> -DWALK_ARGS=${walk_args}
                 -DWALK_OUT=SupervisionTraceData.tla)
        endif()
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${placement_tlc})
        set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT ${ctest_timeout} PROCESSORS ${T_WORKERS})
    endfunction()

    supervision_tlc_test(tlc_supervision_spec Supervision.cfg Supervision pass WORKERS 4 CAP 80)
    supervision_tlc_test(tlc_supervision_rollback SupervisionRollback.cfg Supervision pass WORKERS 4 CAP 80)
    supervision_tlc_test(tlc_supervision_dual_fail SupervisionDualFail.cfg Supervision pass WORKERS 4 CAP 80)
    supervision_tlc_test(tlc_supervision_large SupervisionLarge.cfg Supervision pass WORKERS 8 NIGHTLY CAP 600)

    supervision_tlc_test(tlc_supervision_witness_no_sup_lock witnesses/SNoSupLock.cfg Supervision invariant
                         PROPERTY OneSupervisor)
    supervision_tlc_test(tlc_supervision_witness_no_handover_lock witnesses/SNoHandoverLock.cfg Supervision invariant
                         PROPERTY HandoverSerialized)
    supervision_tlc_test(tlc_supervision_witness_serve_untested witnesses/SServeUntested.cfg Supervision invariant
                         PROPERTY CurrentTested)
    supervision_tlc_test(tlc_supervision_witness_stop_restarts witnesses/SStopRestarts.cfg Supervision action
                         PROPERTY StopStaysStopped)
    supervision_tlc_test(tlc_supervision_witness_drain_stops witnesses/SDrainStops.cfg Supervision action
                         PROPERTY DrainRestartsCurrent)
    supervision_tlc_test(tlc_supervision_witness_no_post_start_rollback witnesses/SNoPostStartRollback.cfg Supervision
                         temporal PROPERTY SettledHealthy)
    supervision_tlc_test(tlc_supervision_witness_ping_pong_rollback witnesses/SPingPongRollback.cfg Supervision action
                         PROPERTY NeverRollsBackToShunned)
    supervision_tlc_test(tlc_supervision_witness_in_memory_state witnesses/SInMemoryState.cfg Supervision action
                         PROPERTY NeverRollsBackToShunned)
    supervision_tlc_test(tlc_supervision_witness_no_halt_bound witnesses/SNoHaltBound.cfg Supervision temporal
                         PROPERTY SupervisorSettles)

    foreach(seed 1 2 3)
        supervision_tlc_test(tlc_supervision_trace_seed_${seed} SupervisionTrace.cfg SupervisionTrace invariant
                             PROPERTY TraceUnfinished WALK ${seed} 350)
    endforeach()
    foreach(fault_seed no-sup-lock:1 no-handover-lock:1 drain-stops:1 stop-restarts:1 no-post-start-rollback:1
                       ping-pong-rollback:2 no-halt-bound:2 in-memory-state:1 serve-untested:1)
        string(REPLACE ":" ";" fs "${fault_seed}")
        list(GET fs 0 fault)
        list(GET fs 1 seed)
        supervision_tlc_test(tlc_supervision_trace_${fault}_rejected SupervisionTrace.cfg SupervisionTrace pass
                             WALK ${seed} 350 --fault ${fault})
    endforeach()
endif()

# Layer 5 migration and rollback-format checks (src/server/migration.h, models/migration):
add_executable(migration_test migration_test.cpp)
target_link_libraries(migration_test PRIVATE nexus_builds)
target_compile_options(migration_test PRIVATE ${warnings})
foreach(group move refusals crash formats)
    add_test(NAME migration_unit_${group} COMMAND migration_test ${group})
    set_tests_properties(migration_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()

if(NOT WIN32)
    add_executable(migration_functional migration_functional.cpp)
    target_link_libraries(migration_functional PRIVATE nexus_builds nexus_durable nexus_config_repo nexus_supervisor)
    target_compile_options(migration_functional PRIVATE ${warnings})
    foreach(case old_to_new live_old_refused crash_resume_and_conflict rollback_formats)
        add_test(NAME migration_functional_${case} COMMAND migration_functional ${case}
                 ${CMAKE_CURRENT_BINARY_DIR}/mig-${case})
        set_tests_properties(migration_functional_${case} PROPERTIES LABELS functional TIMEOUT 120)
    endforeach()

    add_executable(migration_walk migration_walk.cpp)
    target_link_libraries(migration_walk PRIVATE nexus_builds Threads::Threads)
    target_compile_options(migration_walk PRIVATE ${warnings})
    add_dependencies(model_checks migration_walk migration_test)

    set(migration_models "${PROJECT_SOURCE_DIR}/models/migration")
    set(migration_tlc "${PROJECT_SOURCE_DIR}/tests/store/tlc.cmake")
    function(migration_tlc_test name cfg module expect)
        cmake_parse_arguments(T "NIGHTLY" "PROPERTY;WORKERS;CAP" "WALK" ${ARGN})
        if(NOT T_WORKERS)
            set(T_WORKERS 2)
        endif()
        set(label model)
        if(NOT T_CAP)
            set(T_CAP 60)
        endif()
        if(T_NIGHTLY)
            set(label model_nightly)
        endif()
        math(EXPR ctest_timeout "${T_CAP} + 30")
        set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${migration_models} -DMODULE=${module}
                 -DCFG=${cfg} -DEXPECT=${expect} -DWORKERS=${T_WORKERS} -DCAP=${T_CAP})
        if(T_PROPERTY)
            list(APPEND defs -DPROPERTY=${T_PROPERTY})
        endif()
        if(T_WALK)
            string(REPLACE ";" "," walk_args "${T_WALK}")
            list(APPEND defs -DWALK=$<TARGET_FILE:migration_walk> -DWALK_ARGS=${walk_args}
                 -DWALK_OUT=MigrationTraceData.tla)
        endif()
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${migration_tlc})
        set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT ${ctest_timeout} PROCESSORS ${T_WORKERS})
    endfunction()

    # The spec on its small instances and liveness (CutAnywhereResumes):
    # spec: 15,350 states, ~1 s on 4 workers; conflict: 268 states, <1 s; rollback: 2,278 states, <1 s;
    # wide (nightly): 72,813 states, ~8 s on 8 workers.
    migration_tlc_test(tlc_migration_spec Migration.cfg Migration pass WORKERS 4)
    migration_tlc_test(tlc_migration_conflict MigrationConflict.cfg Migration pass WORKERS 4)
    migration_tlc_test(tlc_migration_rollback MigrationRollback.cfg Migration pass WORKERS 4)
    migration_tlc_test(tlc_migration_wide MigrationWide.cfg Migration pass WORKERS 8 NIGHTLY CAP 600)

    # Reversed witnesses: each broken variant must be rejected by the invariant or temporal property named.
    migration_tlc_test(tlc_migration_witness_no_lock witnesses/MNoLock.cfg Migration invariant PROPERTY LockHeld)
    migration_tlc_test(tlc_migration_witness_ignore_live_old witnesses/MIgnoreLiveOld.cfg Migration invariant
                       PROPERTY LiveOldRefused)
    migration_tlc_test(tlc_migration_witness_clobber_new witnesses/MClobberNew.cfg Migration invariant
                       PROPERTY NeverClobbersNewStore)
    migration_tlc_test(tlc_migration_witness_no_sync witnesses/MNoSync.cfg Migration invariant
                       PROPERTY OldDataNeverLost)
    migration_tlc_test(tlc_migration_witness_no_plan witnesses/MNoPlan.cfg Migration invariant
                       PROPERTY OldDataNeverLost)
    migration_tlc_test(tlc_migration_witness_leave_old witnesses/MLeaveOld.cfg Migration invariant PROPERTY MovesOnce)
    migration_tlc_test(tlc_migration_witness_blind_back witnesses/MBlindBack.cfg Migration invariant
                       PROPERTY CurAlwaysReadsStore)
    migration_tlc_test(tlc_migration_witness_multihop witnesses/MMultiHop.cfg Migration invariant
                       PROPERTY CurAlwaysReadsStore)
    migration_tlc_test(tlc_migration_witness_refuse_unknown_change witnesses/MRefuseUnknownChange.cfg Migration
                       invariant PROPERTY RollbackReadable)
    migration_tlc_test(tlc_migration_witness_stuck_lock_file witnesses/MStuckLockFile.cfg Migration temporal
                       PROPERTY CutAnywhereResumes)

    # Trace binding: walks on real POSIX files and locks checked against Migration.tla (seed 1: live old server on
    # server.sock; seed 2: live old process holding store flock; seed 3: cold migration + switches/rollbacks; seed 4:
    # conflict refusal).
    foreach(seed 1 2 3 4)
        migration_tlc_test(tlc_migration_trace_seed_${seed} MigrationTrace.cfg MigrationTrace invariant
                           PROPERTY TraceUnfinished WALK ${seed} 300)
    endforeach()

    # Reversed fault walks: each diverges from Migration.tla (Bug = "none"), so no behaviour reaches the end of the
    # trace (`pass` is rejection).
    foreach(fault_seed no-lock:1 ignore-live-old:1 clobber-new:1 no-sync:3 no-plan:3 leave-old:3
                       blind-back:1 multihop:1 refuse-unknown-change:1)
        string(REPLACE ":" ";" fs "${fault_seed}")
        list(GET fs 0 fault)
        list(GET fs 1 seed)
        migration_tlc_test(tlc_migration_trace_${fault}_rejected MigrationTrace.cfg MigrationTrace pass
                           WALK ${seed} 300 --fault ${fault})
    endforeach()
endif()
