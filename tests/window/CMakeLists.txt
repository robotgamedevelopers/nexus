# The window's unit tests: pure logic only (no OS, no clock, no window, no network).
add_executable(nexus_window_tests test_window.cpp)
target_compile_options(nexus_window_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_window_tests PRIVATE nexus_window_logic)
add_test(NAME window_unit COMMAND nexus_window_tests)
set_tests_properties(window_unit PROPERTIES LABELS unit)

# The window's input, headless: the view over a Prax UI context with no window and no GPU, fed key events. Text is set
# in the system's fonts; the clock is made up.
add_executable(nexus_input_tests test_input.cpp)
target_compile_options(nexus_input_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_input_tests PRIVATE nexus_view)
add_test(NAME window_input COMMAND nexus_input_tests)
set_tests_properties(window_input PROPERTIES LABELS unit)

# A terminal tab with a real shell on a pseudoterminal: a functional test (a process, real time). Its working directory
# is its own, under the build tree.
add_executable(nexus_terminal_tests test_terminal.cpp)
target_compile_options(nexus_terminal_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_terminal_tests PRIVATE nexus_terminals)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/terminal_work)
add_test(NAME window_terminal COMMAND nexus_terminal_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/terminal_work)
set_tests_properties(window_terminal PROPERTIES LABELS functional TIMEOUT 60)
