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
    set(conformance_cases exchange refused not_json order send_while_open eof_while_open)
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
                    "${PROJECT_SOURCE_DIR}/drivers/claude/dist/test/fake-main.js"
                WORKING_DIRECTORY "${conformance_dir}")
            set_tests_properties(driver_conformance_claude_${case} PROPERTIES LABELS claude_driver TIMEOUT 60)
        endforeach()
    endif()
endif()

# The unit test runs under the sanitizers with the model's programs (cmake --build <dir> --target model_checks).
add_dependencies(model_checks driver_protocol_test)

foreach(group roundtrip encode_refusals property chunking refusals strings numbers forward limit)
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
    foreach(case exchange refused raw no_block ignore_sigterm order send_while_open hang options starts ignore_eof)
        add_test(NAME driver_fake_${case} COMMAND driver_fake_test $<TARGET_FILE:nexus_fake_driver> ${case}
            WORKING_DIRECTORY "${functional_dir}")
        set_tests_properties(driver_fake_${case} PROPERTIES LABELS functional TIMEOUT 60)
    endforeach()
endif()
