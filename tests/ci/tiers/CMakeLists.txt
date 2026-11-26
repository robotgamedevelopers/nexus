# Planted cases for cmake/tiers.cmake (.github/tiers-selftest.sh): a unit test, a slow unit test the table moves to the
# nightly, a functional test, a happy path the table moves to every pull request, and a test with no label. The happy
# path needs pr_tool and the functional test nightly_tool (NEXUS_NEEDS): tier_pr_build makes pr_tool alone. With
# PLANTED_UNBUILT, a unit test runs a file in the build tree that no target builds, which must fail the configure.
cmake_minimum_required(VERSION 3.28)
project(tiers_planted LANGUAGES NONE)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/test_targets.cmake)
enable_testing()
option(PLANTED_ON "" ON)
option(PLANTED_OFF "" OFF)
option(PLANTED_UNBUILT "" OFF)
add_custom_target(pr_tool COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/pr_tool.made")
add_custom_target(nightly_tool COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/nightly_tool.made")
add_test(NAME fast_unit COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(fast_unit PROPERTIES LABELS unit)
add_test(NAME slow_unit COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(slow_unit PROPERTIES LABELS unit)
if(PLANTED_UNBUILT)
    add_test(NAME unbuilt_unit COMMAND "${CMAKE_BINARY_DIR}/unbuilt_program")
    set_tests_properties(unbuilt_unit PROPERTIES LABELS unit)
endif()
add_subdirectory(sub)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/tiers.cmake)
