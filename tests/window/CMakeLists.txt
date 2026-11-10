# The window's unit tests: pure logic only (no OS, no clock, no window, no network).
add_executable(nexus_window_tests test_window.cpp)
target_compile_options(nexus_window_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_window_tests PRIVATE nexus_window_logic)
add_test(NAME window_unit COMMAND nexus_window_tests)
set_tests_properties(window_unit PROPERTIES LABELS unit)
