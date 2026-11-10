# The window's unit tests: pure logic only (no OS, no clock, no window, no network).
add_executable(nexus_window_tests test_window.cpp)
target_compile_options(nexus_window_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_window_tests PRIVATE nexus_window_logic)
add_test(NAME window_unit COMMAND nexus_window_tests)
set_tests_properties(window_unit PROPERTIES LABELS unit)

# Running a command (src/friends.h runCommand): real child processes and a real clock.
add_executable(nexus_command_tests test_command.cpp)
target_compile_options(nexus_command_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_command_tests PRIVATE nexus_window_logic)
add_test(NAME window_command COMMAND nexus_command_tests)
set_tests_properties(window_command PROPERTIES LABELS functional TIMEOUT 30)

# The window's input, headless: the view over a Prax UI context with no window and no GPU, fed key events. Text is set
# in the system's fonts; the clock is made up.
add_executable(nexus_input_tests test_input.cpp)
target_compile_options(nexus_input_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_input_tests PRIVATE nexus_view)
add_test(NAME window_input COMMAND nexus_input_tests)
set_tests_properties(window_input PROPERTIES LABELS unit)
