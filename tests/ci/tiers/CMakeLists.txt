# Planted cases for cmake/tiers.cmake (.github/tiers-selftest.sh): a unit test, a slow unit test the table moves to the
# nightly, a functional test, a happy path the table moves to every pull request, and a test with no label.
cmake_minimum_required(VERSION 3.28)
project(tiers_planted LANGUAGES NONE)
enable_testing()
option(PLANTED_ON "" ON)
option(PLANTED_OFF "" OFF)
add_test(NAME fast_unit COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(fast_unit PROPERTIES LABELS unit)
add_test(NAME slow_unit COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(slow_unit PROPERTIES LABELS unit)
add_subdirectory(sub)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/tiers.cmake)
