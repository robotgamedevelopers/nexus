# Tests of the Tailscale shim (src/tailscale/nts.h, docs/tailscale.md), built only with -DNEXUS_TAILSCALE=ON. Every test
# here is labelled tailscale too, so that CI's tailscale jobs run exactly these (ctest -L tailscale):
#   unit           tailscale_unit_version: the versions the shim reports are the pins. No OS, no time, no network.
#   functional     tailscale_functional_state_dir, tailscale_functional_refusals: the state directory and its log file
#                  on the real filesystem, in a directory of the test's own, and every refusal that stops before the
#                  network. No socket, no network, no clock.
# Nothing here starts a node that reaches Tailscale's control server: that is `tailscale_test start_needs_login <dir>`,
# run by hand on a bench (docs/tailscale.md).

add_executable(tailscale_test tailscale_test.cpp)
target_compile_options(tailscale_test PRIVATE ${NEXUS_WARNINGS})
nexus_link_tailscale(tailscale_test)
if(WIN32)
    target_link_libraries(tailscale_test PRIVATE advapi32)
endif()

# What CI builds in one step: the test program, go vet and the licence notices' check (cmake/tailscale.cmake). They
# are independent, so a parallel build runs them side by side (CMake's Makefiles run separate --target goals in turn).
add_custom_target(tailscale_checks)
add_dependencies(tailscale_checks tailscale_test nexus_tailscale_vet nexus_tailscale_notices_check)

add_test(NAME tailscale_unit_version COMMAND tailscale_test version)
set_tests_properties(tailscale_unit_version PROPERTIES LABELS "unit;tailscale" TIMEOUT 30)

foreach(group state_dir refusals refuses_inherited_key)
    set(work ${CMAKE_CURRENT_BINARY_DIR}/work_${group})
    file(MAKE_DIRECTORY ${work})
    add_test(NAME tailscale_functional_${group} COMMAND tailscale_test ${group} WORKING_DIRECTORY ${work})
    set_tests_properties(tailscale_functional_${group} PROPERTIES LABELS "functional;tailscale" TIMEOUT 30)
endforeach()
# A fake key, in the environment from the process's start.
set_tests_properties(tailscale_functional_refuses_inherited_key PROPERTIES
    ENVIRONMENT "TS_AUTHKEY=a-value-that-must-not-be-shown")
