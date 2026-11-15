# Tests of the Tailscale shim (src/tailscale/nts.h, docs/tailscale.md), built only with -DNEXUS_TAILSCALE=ON. Every test
# here is labelled tailscale too, so that CI's tailscale jobs run exactly these (ctest -L tailscale):
#   unit           no OS, no time, no network: tailscale_unit_lifecycle (the node's state code, src/tailscale/lifecycle,
#                  in Go: the state mapping, the start's checks, the record of the control server, the poll's settling,
#                  the state directory table), and the test program's unit groups (version, names, handles,
#                  start_checks, panics_unit)
#   functional     the real filesystem in a directory of the test's own, and nodes on this machine only: offline nodes
#                  (a control server on 127.0.0.1 that refuses at once), and Tailscale's in-tree test control server run
#                  inside the test program on 127.0.0.1 with its own DERP relay and STUN server
# Nothing here reaches another machine, Tailscale's hosted service or a real tailnet: that is
# `tailscale_test start_needs_login <dir>`, run by hand on a bench (docs/tailscale.md). The test program links the
# shim's test build (TESTHOOKS); nexus's build is built beside it (tailscale_checks), so CI builds both.

add_executable(tailscale_test tailscale_test.cpp)
target_compile_options(tailscale_test PRIVATE ${NEXUS_WARNINGS})
nexus_link_tailscale(tailscale_test TESTHOOKS)
if(WIN32)
    target_link_libraries(tailscale_test PRIVATE advapi32)
endif()

# What CI builds in one step: the test program, nexus's build of the shim, go vet and the licence notices' check
# (cmake/tailscale.cmake). They are independent, so a parallel build runs them side by side (CMake's Makefiles run
# separate --target goals in turn).
add_custom_target(tailscale_checks)
add_dependencies(tailscale_checks tailscale_test nexus_tailscale_shim nexus_tailscale_vet nexus_tailscale_notices_check
    nexus_tailscale_environment_check)

# The lifecycle module's Go tests, with the pinned Go (a module of its own, with no requirements).
add_test(NAME tailscale_unit_lifecycle
    COMMAND ${CMAKE_COMMAND} -E env ${nexus_go_env} CGO_ENABLED=0 ${NEXUS_GO} test -count=1 ./...
    WORKING_DIRECTORY ${nexus_ts_source}/lifecycle)
set_tests_properties(tailscale_unit_lifecycle PROPERTIES LABELS "unit;tailscale" TIMEOUT 120)

foreach(group version names handles start_checks panics_unit)
    set(work ${CMAKE_CURRENT_BINARY_DIR}/work_${group})
    file(MAKE_DIRECTORY ${work})
    add_test(NAME tailscale_unit_${group} COMMAND tailscale_test ${group} WORKING_DIRECTORY ${work})
    set_tests_properties(tailscale_unit_${group} PROPERTIES LABELS "unit;tailscale" TIMEOUT 30)
endforeach()

# The groups, here and in .github/workflows/tailscale.yml's list for macOS's x86_64 slice.
set(NEXUS_TAILSCALE_FUNCTIONAL state_dir refusals refuses_inherited_key lock lock_two_process panics log_cap quiet
    unreachable certificate watch threads sign_in tailnet restart expiry control_down auth_key long_sign_in
    control_error control_blackhole close_each_other)
foreach(group ${NEXUS_TAILSCALE_FUNCTIONAL})
    set(work ${CMAKE_CURRENT_BINARY_DIR}/work_${group})
    file(MAKE_DIRECTORY ${work})
    add_test(NAME tailscale_functional_${group} COMMAND tailscale_test ${group} WORKING_DIRECTORY ${work})
    set_tests_properties(tailscale_functional_${group} PROPERTIES LABELS "functional;tailscale" TIMEOUT 120)
endforeach()
# A fake key, in the environment from the process's start.
set_tests_properties(tailscale_functional_refuses_inherited_key PROPERTIES
    ENVIRONMENT "TS_AUTHKEY=a-value-that-must-not-be-shown")
