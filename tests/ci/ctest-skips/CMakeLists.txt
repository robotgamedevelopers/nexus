# Planted cases for .github/ctest.sh (ctest-selftest.sh runs them): one test that passes, one that skips by its
# SKIP_RETURN_CODE, one disabled. The wrapper must pass the first and fail the other two, and fail a selection of none.
cmake_minimum_required(VERSION 3.28)
project(ctest_skips NONE)
enable_testing()
add_test(NAME passes COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(passes PROPERTIES LABELS passes)
add_test(NAME skips COMMAND sh -c "exit 77")
set_tests_properties(skips PROPERTIES LABELS skips SKIP_RETURN_CODE 77)
add_test(NAME disabled COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(disabled PROPERTIES LABELS disabled DISABLED TRUE)
