# Tests of the driver protocol (src/driver/), with two labels:
#   unit        driver_protocol_test: every message's round trip, the encoder refusing what the decoder refuses (with a
#               property test over random messages), the reader over every chunking, each refusal with its cause,
#               unknown fields and kinds, and the line limit at its edges; logic only (one ctest entry per group, so
#               they run in parallel)
#   functional  driver_conformance_fake_*: the protocol's conformance cases (conformance_test.cpp) against the fake
#               driver, which proves the cases
#   claude_driver driver_conformance_claude_*: the same cases against the Claude driver (drivers/claude) over its
#               scripted query; needs Node and the driver built (npm ci && npm run build in drivers/claude), so it is a
#               label of its own that the claude-driver CI job runs
#   functional  driver_fake_test: the fake driver (nexus-fake-driver, the stand-in for a harness's driver that the
#               server's tests start) as a real child process over pipes, in a working directory of its own

set(warnings ${NEXUS_WARNINGS})

if(APPLE)
    add_test(NAME driver_bus_listener_idle
        COMMAND /usr/bin/python3 "${CMAKE_CURRENT_SOURCE_DIR}/bus_listener_idle.py"
                "${CMAKE_SOURCE_DIR}/drivers/claude/bin/bus/nova-bus")
    set_tests_properties(driver_bus_listener_idle PROPERTIES LABELS functional TIMEOUT 15)
endif()

add_executable(driver_protocol_test protocol_test.cpp)
target_link_libraries(driver_protocol_test PRIVATE nexus_driver)
target_compile_options(driver_protocol_test PRIVATE ${warnings})

# The fake driver is a POSIX program, as nexus-server's drivers are (not built natively on Windows, where nexus-server runs inside WSL2: CMakeLists.txt NEXUS_LINK).
if(NOT WIN32)
    add_executable(nexus_fake_driver fake_driver.cpp)
    target_link_libraries(nexus_fake_driver PRIVATE nexus_driver)
    target_compile_options(nexus_fake_driver PRIVATE ${warnings})
    set_target_properties(nexus_fake_driver PROPERTIES OUTPUT_NAME nexus-fake-driver)
endif()

# Protocol conformance of a driver as a child process (POSIX: posix_spawn and pipes).
if(NOT WIN32)
    add_executable(driver_conformance_test conformance_test.cpp)
    target_link_libraries(driver_conformance_test PRIVATE nexus_driver)
    target_compile_options(driver_conformance_test PRIVATE ${warnings})
    add_dependencies(driver_conformance_test nexus_fake_driver)

    set(conformance_dir "${CMAKE_CURRENT_BINARY_DIR}/conformance")
    file(MAKE_DIRECTORY "${conformance_dir}")
    set(conformance_cases exchange refused not_json order send_while_open eof_while_open
        secret_ask secret_without_server)
    # Steering a turn: only the fake names steer, follow-up and settled (the Claude driver's hello names none yet).
    foreach(case steer_exchange steer_stopped steer_crossed steer_unnamed steer_unopened_turn steer_id_order
                 steer_paused)
        add_test(NAME driver_conformance_fake_${case}
            COMMAND driver_conformance_test ${case} fake $<TARGET_FILE:nexus_fake_driver>
            WORKING_DIRECTORY "${conformance_dir}")
        set_tests_properties(driver_conformance_fake_${case} PROPERTIES LABELS functional TIMEOUT 60)
    endforeach()
    foreach(case ${conformance_cases})
        add_test(NAME driver_conformance_fake_${case}
            COMMAND driver_conformance_test ${case} fake $<TARGET_FILE:nexus_fake_driver>
            WORKING_DIRECTORY "${conformance_dir}")
        set_tests_properties(driver_conformance_fake_${case} PROPERTIES LABELS functional TIMEOUT 60)
    endforeach()

    find_program(NEXUS_NODE node)
    if(NEXUS_NODE)
        foreach(case ${conformance_cases})
            add_test(NAME driver_conformance_claude_${case}
                COMMAND driver_conformance_test ${case} prompted ${NEXUS_NODE}
                    "${PROJECT_SOURCE_DIR}/drivers/claude/dist/test/fake-main.js" --account conformance
                WORKING_DIRECTORY "${conformance_dir}")
            set_tests_properties(driver_conformance_claude_${case} PROPERTIES LABELS claude_driver TIMEOUT 60)
        endforeach()
    endif()
endif()

# The Claude driver's turns (models/ClaudeDriver.tla), label model: the spec on its two per-commit instances, each in
# a CI shard of its own (SHARD: capped at 80 s, with no Node build); a reversed witness per property, each a bug TLC
# must reject; and the driver's traces, each a behaviour of the spec: its unit tests' and its walk's
# (drivers/claude/test/walk.ts) over several seeds, with four faulty walks rejected, in claude-rest (node: true).
tlc_test(tlc_claude_driver ClaudeDriver MCClaudeDriver.cfg pass WORKERS 4 SHARD)
tlc_test(tlc_claude_driver_secrets ClaudeDriver MCClaudeDriverSecrets.cfg pass WORKERS 4 SHARD)
# Nightly: both settings (model and effort), two turns: 127,240 states, 16 s on four workers in CI; and three turns
# with two secret asks and two setting values (197,452 states, 20 s on eight workers).
tlc_test(tlc_claude_driver_two_settings ClaudeDriver MCClaudeDriverTwo.cfg pass WORKERS 8 NIGHTLY)
tlc_test(tlc_claude_driver_secrets_wide ClaudeDriver MCClaudeDriverSecretsWide.cfg pass WORKERS 8 NIGHTLY)
tlc_test(tlc_claude_driver_witness_done_left_open WDrvDoneLeftOpen WDrvDoneLeftOpen.cfg invariant PROPERTY OneDone)
tlc_test(tlc_claude_driver_witness_apply_during_interrupt WDrvApplyDuringInterrupt WDrvApplyDuringInterrupt.cfg
         invariant PROPERTY StopNeverLater)
tlc_test(tlc_claude_driver_witness_ask_blocks_stop WDrvAskBlocksStop WDrvAskBlocksStop.cfg invariant
         PROPERTY AskOnlyRunning)
tlc_test(tlc_claude_driver_witness_refusal_in_block WDrvRefusalInBlock WDrvRefusalInBlock.cfg action
         PROPERTY NoDoneInBlock)
tlc_test(tlc_claude_driver_witness_ask_without_secrets WDrvAskWithoutSecrets WDrvAskWithoutSecrets.cfg action
         PROPERTY AskGate)
tlc_test(tlc_claude_driver_witness_two_asks_open WDrvTwoAsksOpen WDrvTwoAsksOpen.cfg action PROPERTY AskGate)
tlc_test(tlc_claude_driver_witness_stale_secret WDrvStaleSecret WDrvStaleSecret.cfg action PROPERTY SecretMatchesAsk)
tlc_test(tlc_claude_driver_witness_lost_start_settings WDrvLostStartSettings WDrvLostStartSettings.cfg action
         PROPERTY SettingsNeverChangeOpen)
tlc_test(tlc_claude_driver_witness_stop_drops_settings WDrvStopDropsSettings WDrvStopDropsSettings.cfg action
         PROPERTY SettingsFromNextSend)
tlc_test(tlc_claude_driver_witness_early_stop_ignored WDrvEarlyStopIgnored WDrvEarlyStopIgnored.cfg action
         PROPERTY StopEndsWhatRuns)
tlc_test(tlc_claude_driver_witness_stop_latched WDrvStopLatched WDrvStopLatched.cfg action PROPERTY NoLatchedStop)
tlc_test(tlc_claude_driver_witness_stopped_lost_hangs WDrvStoppedLostHangs WDrvStoppedLostHangs.cfg liveness)
tlc_test(tlc_claude_driver_trace_unit ClaudeDriverTrace ClaudeDriverTrace.cfg pass DRIVERTESTS)
foreach(seed 1 2 3 4)
    tlc_test(tlc_claude_driver_trace_seed_${seed} ClaudeDriverTrace ClaudeDriverTrace.cfg pass DRIVERWALK
             SEED ${seed} STEPS 600)
endforeach()
# The binding can fail: a driver that drops a stopped turn's settings (seed 1 stops a turn that carries settings), one
# whose refusal leaves a text block open (seed 3 refuses mid-block), one that keeps a stop for a later turn, and one
# that writes ask-secret without a server line naming secrets.
tlc_test(tlc_claude_driver_trace_faulty_settings_rejected ClaudeDriverTrace ClaudeDriverTrace.cfg invariant
         PROPERTY TraceMatches DRIVERWALK SEED 1 STEPS 600 FAULTY settings)
tlc_test(tlc_claude_driver_trace_faulty_refusal_rejected ClaudeDriverTrace ClaudeDriverTrace.cfg invariant
         PROPERTY TraceMatches DRIVERWALK SEED 3 STEPS 600 FAULTY refusal)
tlc_test(tlc_claude_driver_trace_faulty_latch_rejected ClaudeDriverTrace ClaudeDriverTrace.cfg invariant
         PROPERTY TraceMatches DRIVERWALK SEED 1 STEPS 600 FAULTY latch)
tlc_test(tlc_claude_driver_trace_faulty_secret_rejected ClaudeDriverTrace ClaudeDriverTrace.cfg invariant
         PROPERTY TraceMatches DRIVERWALK SEED 1 STEPS 600 FAULTY secret)

# Steering a turn (models/Steering.tla, docs/driver-protocol.md "Steering a turn"). All label model, which CI runs
# nightly only (the model shard supervisor-rest takes tlc_supervisor_*; tests/server builds steering_walk with
# supervisor_walk), none per pull request: the spec on two small instances within the 60 s cap (Steering.cfg,
# SteeringThree.cfg) and on every set of features (SteeringWide.cfg, SteeringWideRest.cfg; model_nightly); a reversed
# witness per property, each a planted bug TLC must reject; and the traces of steering_walk (the server's real
# supervisor and the driver's real side, src/driver/steering.h, over encoded lines) over several seeds, each a behaviour
# of the spec, with planted faults of either side rejected. The sanitizer jobs run steering_walk itself per pull request
# (.github/workflows/ci.yml).
if(NEXUS_SERVER_PARTS)
    add_executable(steering_walk steering_walk.cpp)
    target_link_libraries(steering_walk PRIVATE nexus_supervisor)
    target_compile_options(steering_walk PRIVATE ${warnings})
    add_dependencies(model_checks steering_walk)
endif()
tlc_test(tlc_supervisor_steering Steering Steering.cfg pass WORKERS 4)
tlc_test(tlc_supervisor_steering_three Steering SteeringThree.cfg pass WORKERS 4)
tlc_test(tlc_supervisor_steering_wide Steering SteeringWide.cfg pass WORKERS 8 NIGHTLY)
tlc_test(tlc_supervisor_steering_wide_rest Steering SteeringWideRest.cfg pass WORKERS 8 NIGHTLY)
foreach(witness Twice:invariant:AtMostOnce Drop:invariant:NeverLost EarlyFollow:action:FollowAtRunEnd
                Silent:invariant:ViewMatches Unnamed:invariant:NamedOnly Stale:invariant:AtMostOnce
                SettleEarly:action:SettleEmpty SendAtSettled:invariant:DriverKeptRules Unbounded:invariant:WaitingFits
                Loop:liveness:)
    string(REPLACE ":" ";" parts "${witness}")
    list(GET parts 0 fault)
    list(GET parts 1 expect)
    list(GET parts 2 property)
    string(TOLOWER "${fault}" lower)
    if(property)
        tlc_test(tlc_supervisor_steering_witness_${lower} Steering witnesses/WSteer${fault}.cfg ${expect}
                 PROPERTY ${property})
    else()
        tlc_test(tlc_supervisor_steering_witness_${lower} Steering witnesses/WSteer${fault}.cfg ${expect})
    endif()
endforeach()
# The trace checks run tests/model/tlc.cmake with steering_walk as its WALK (its arguments: SEED, STEPS, and the mode
# in AIS's place), as tlc_test does for the walks it names.
function(steering_trace_test name seed steps mode expect)
    if(NOT NEXUS_SERVER_PARTS)
        return()
    endif()
    set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${PROJECT_SOURCE_DIR}/models
             -DMODULE=SteeringTrace -DCFG=SteeringTrace.cfg -DEXPECT=${expect} -DWORKERS=2 -DCAP=60
             -DWALK=$<TARGET_FILE:steering_walk> -DDATA=SteeringTraceData -DSEED=${seed} -DSTEPS=${steps}
             -DAIS=${mode})
    if(ARGN)
        list(APPEND defs -DPROPERTY=${ARGN})
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${PROJECT_SOURCE_DIR}/tests/model/tlc.cmake)
    set_tests_properties(${name} PROPERTIES LABELS model TIMEOUT 90 PROCESSORS 2)
endfunction()
# Seeds below 1000 name all three features; 1000 to 1003 name the other sets (nothing, settled only, steer and settled,
# follow-up and settled).
foreach(seed RANGE 1 20)
    steering_trace_test(tlc_supervisor_steering_trace_seed_${seed} ${seed} 2000 ok pass)
endforeach()
foreach(seed 1000 1001 1002 1003)
    steering_trace_test(tlc_supervisor_steering_trace_seed_${seed} ${seed} 2000 ok pass)
endforeach()
steering_trace_test(tlc_supervisor_steering_trace_faulty_silent_rejected 4 600 silent invariant TraceMatches)
steering_trace_test(tlc_supervisor_steering_trace_faulty_early_follow_rejected 4 600 early-follow invariant
                    TraceMatches)
# A server that writes into a turn after reading its settled (Supervisor::into without its settled check).
steering_trace_test(tlc_supervisor_steering_trace_faulty_write_settled_rejected 1 2000 write-settled invariant
                    TraceMatches)

# The unit test runs under the sanitizers with the model's programs (cmake --build <dir> --target model_checks).
add_dependencies(model_checks driver_protocol_test)

foreach(group roundtrip encode_refusals property chunking refusals strings numbers forward secrets limit steering
              steering_driver)
    add_test(NAME driver_unit_${group} COMMAND driver_protocol_test ${group})
    set_tests_properties(driver_unit_${group} PROPERTIES LABELS unit TIMEOUT 60)
endforeach()

# The functional test starts the fake driver through Prax's lib/process: with the window's logic (NEXUS_WINDOW_LOGIC in
# CMakeLists.txt), where lib/process links, and with the fake driver (not on Windows).
if(NEXUS_WINDOW_LOGIC AND NOT WIN32)
    add_executable(driver_fake_test fake_driver_test.cpp)
    target_link_libraries(driver_fake_test PRIVATE nexus_driver prax::process)
    target_compile_options(driver_fake_test PRIVATE ${warnings})
    add_dependencies(driver_fake_test nexus_fake_driver)

    set(functional_dir "${CMAKE_CURRENT_BINARY_DIR}/functional")
    file(MAKE_DIRECTORY "${functional_dir}")
    # One entry per case, so they run in parallel; each case uses files of its own names.
    foreach(case exchange refused raw no_block ignore_sigterm order send_while_open hang options starts ignore_eof stream
            prompts)
        add_test(NAME driver_fake_${case} COMMAND driver_fake_test $<TARGET_FILE:nexus_fake_driver> ${case}
            WORKING_DIRECTORY "${functional_dir}")
        set_tests_properties(driver_fake_${case} PROPERTIES LABELS functional TIMEOUT 60)
    endforeach()
endif()
