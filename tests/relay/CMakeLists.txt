# Tests of the relay between the native window on Windows and the link inside WSL2 (src/relay/relay_wire.h):
#   unit        relay_unit: the frames, the hellos, actions and history requests down, the link's state up as diffs,
#               and both ends across a relay that ended; logic only, built and run everywhere
#   functional  wsl2_link (Windows only, not a ctest: tests/relay/wsl2_link.sh runs it): the window's port (src/relay/relay_port.h) starts
#               nexus-link-relay inside WSL2 through wsl.exe, connects to a nexus-server there with a home of its own,
#               sends a prompt and sees it, and the fake driver's reply, in the server's state. Like wsl2_terminal it
#               needs a WSL2 distribution, which CI's Windows services have not: CI runs it from inside captain's WSL2
#               through interop (.github/workflows/nightly.yml, wsl2-link), with the relay, the server and the fake driver
#               of the Linux build there.

add_executable(relay_unit relay_test.cpp)
target_link_libraries(relay_unit PRIVATE nexus_relay_wire)
target_compile_options(relay_unit PRIVATE ${NEXUS_WARNINGS})
add_test(NAME relay_unit COMMAND relay_unit)
set_tests_properties(relay_unit PROPERTIES LABELS unit TIMEOUT 30)

if(WIN32)
    add_executable(nexus_wsl2_link_tests wsl2_link.cpp)
    target_link_libraries(nexus_wsl2_link_tests PRIVATE nexus_relay_port)
    target_compile_options(nexus_wsl2_link_tests PRIVATE ${NEXUS_WARNINGS})
endif()

# The window's side of the relays' life (src/relay/relay_life.h), models/RelayLife.tla: the spec with every property on
# three relays and three actions, and on six relays (the backoff to its cap); its reversed witnesses (the unacknowledged
# actions written again to the next relay, lost silently, two relays at once, no backoff, a backoff with no cap, an
# acknowledgement of what was only delivered, the cut never told, a backoff that never ends); and the code's walks
# (relay_walk) checked against it, with three faulty walks rejected.
if(NEXUS_SERVER_PARTS)
    add_executable(relay_walk relay_walk.cpp)
    target_link_libraries(relay_walk PRIVATE nexus_relay_wire)
    target_compile_options(relay_walk PRIVATE ${NEXUS_WARNINGS})
    add_dependencies(model_checks relay_walk)
endif()
tlc_test(tlc_relay_spec         RelayLife MCRelayLife.cfg        pass WORKERS 4)
tlc_test(tlc_relay_spec_backoff RelayLife MCRelayLifeBackoff.cfg pass WORKERS 4)
tlc_test(tlc_relay_witness_resend    WRelayResend    WRelayResend.cfg    invariant PROPERTY AppliedOnce)
tlc_test(tlc_relay_witness_silent    WRelaySilent    WRelaySilent.cfg    invariant PROPERTY Covered)
tlc_test(tlc_relay_witness_two       WRelayTwo       WRelayTwo.cfg       invariant PROPERTY OneRelay)
tlc_test(tlc_relay_witness_no_backoff WRelayNoBackoff WRelayNoBackoff.cfg action PROPERTY WaitsBackoff)
tlc_test(tlc_relay_witness_uncapped  WRelayUncapped  WRelayUncapped.cfg  invariant PROPERTY BackoffOK)
tlc_test(tlc_relay_witness_ack_early WRelayAckEarly  WRelayAckEarly.cfg  invariant PROPERTY AckTrue)
tlc_test(tlc_relay_witness_untold    WRelayUntold    WRelayUntold.cfg    liveness)
tlc_test(tlc_relay_witness_stuck     WRelayStuck     WRelayStuck.cfg     liveness)
foreach(seed 1 2 3 4)
    tlc_test(tlc_relay_trace_seed_${seed} RelayLifeTrace RelayLifeTrace.cfg pass RELAYWALK SEED ${seed} STEPS 1000)
endforeach()
foreach(fault resend early-end ack-delivered)
    tlc_test(tlc_relay_trace_faulty_${fault}_rejected RelayLifeTrace RelayLifeTrace.cfg deadlock
             RELAYWALK SEED 1 STEPS 1000 FAULTY ${fault})
endforeach()
