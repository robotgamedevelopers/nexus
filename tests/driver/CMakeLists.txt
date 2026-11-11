# Tests of the driver protocol (src/driver/), with two labels:
#   unit        driver_protocol_test: every message's round trip, the reader over every chunking, each refusal with its
#               cause, unknown fields and kinds, and the line limit at its edges; logic only (one ctest entry per group,
#               so they run in parallel)
#   functional  driver_fake_test: the fake driver (nexus-fake-driver, the stand-in for a harness's driver that the
#               server's tests start) as a real child process over pipes, in a working directory of its own

set(warnings -Wall -Wextra -Wpedantic -Werror)

add_executable(driver_protocol_test protocol_test.cpp)
target_link_libraries(driver_protocol_test PRIVATE nexus_driver)
target_compile_options(driver_protocol_test PRIVATE ${warnings})

add_executable(nexus_fake_driver fake_driver.cpp)
target_link_libraries(nexus_fake_driver PRIVATE nexus_driver)
target_compile_options(nexus_fake_driver PRIVATE ${warnings})
set_target_properties(nexus_fake_driver PROPERTIES OUTPUT_NAME nexus-fake-driver)

add_executable(driver_fake_test fake_driver_test.cpp)
target_link_libraries(driver_fake_test PRIVATE nexus_driver prax::process)
target_compile_options(driver_fake_test PRIVATE ${warnings})
add_dependencies(driver_fake_test nexus_fake_driver)

# The unit test runs under the sanitizers with the model's programs (cmake --build <dir> --target model_checks).
add_dependencies(model_checks driver_protocol_test)

foreach(group roundtrip encode_refusals chunking refusals forward limit)
    add_test(NAME driver_unit_${group} COMMAND driver_protocol_test ${group})
    set_tests_properties(driver_unit_${group} PROPERTIES LABELS unit TIMEOUT 60)
endforeach()

set(functional_dir "${CMAKE_CURRENT_BINARY_DIR}/functional")
file(MAKE_DIRECTORY "${functional_dir}")
add_test(NAME driver_fake COMMAND driver_fake_test $<TARGET_FILE:nexus_fake_driver>
    WORKING_DIRECTORY "${functional_dir}")
set_tests_properties(driver_fake PROPERTIES LABELS functional TIMEOUT 60)
