# The window's unit tests: pure logic only (no OS, no clock, no window, no network).
add_executable(nexus_window_tests test_window.cpp)
target_compile_options(nexus_window_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_window_tests PRIVATE nexus_window_logic)
add_test(NAME window_unit COMMAND nexus_window_tests)
set_tests_properties(window_unit PROPERTIES LABELS unit)

# The sprint tab's logic (src/sprint.h): the board's address from the sprint's record through a fake command runner,
# event routing, and the page's prelude and style as text. Pure logic.
add_executable(nexus_sprint_tests test_sprint.cpp)
target_compile_options(nexus_sprint_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_sprint_tests PRIVATE nexus_window_logic)
add_test(NAME sprint_unit COMMAND nexus_sprint_tests)
set_tests_properties(sprint_unit PROPERTIES LABELS unit)

# The sprint board's scroll glide (src/scroll_glide.h): the ease, new targets, the ends, held keys. Time is an argument.
add_executable(nexus_scroll_glide_tests test_scroll_glide.cpp)
target_compile_options(nexus_scroll_glide_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_scroll_glide_tests PRIVATE nexus_window_logic)
add_test(NAME scroll_glide_unit COMMAND nexus_scroll_glide_tests)
set_tests_properties(scroll_glide_unit PROPERTIES LABELS unit)

# The sprint tab's page (src/board.h) headless over Prax's HTML stack: a fixture board served from memory on Prax's null
# network (no socket), QuickJS scripts and the system's fonts, with made-up clocks. A functional test, in a working
# directory of its own.
add_executable(nexus_board_tests test_board.cpp)
target_compile_options(nexus_board_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_board_tests PRIVATE nexus_board nexus_view)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/board_work)
add_test(NAME sprint_board COMMAND nexus_board_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/board_work)
set_tests_properties(sprint_board PROPERTIES LABELS functional TIMEOUT 60)

# Running a command (src/friends.h runCommand): real child processes and a real clock.
add_executable(nexus_command_tests test_command.cpp)
target_compile_options(nexus_command_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_command_tests PRIVATE nexus_window_logic)
add_test(NAME window_command COMMAND nexus_command_tests)
set_tests_properties(window_command PROPERTIES LABELS functional TIMEOUT 30)

# The tab order's file store: a real file, in a directory of the test's own.
add_executable(nexus_tab_order_file_tests test_tab_order_file.cpp)
target_compile_options(nexus_tab_order_file_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_tab_order_file_tests PRIVATE nexus_window_logic)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tab_order_file)
add_test(NAME tab_order_file COMMAND nexus_tab_order_file_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tab_order_file)
set_tests_properties(tab_order_file PROPERTIES LABELS functional TIMEOUT 30)

# The window's input, headless: the view over a Prax UI context with no window and no GPU, fed key events. The clock is
# made up, but text is set in the system's fonts, an outside service: so a functional test, not a unit test.
add_executable(nexus_input_tests test_input.cpp)
target_compile_options(nexus_input_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_input_tests PRIVATE nexus_view)
add_test(NAME window_input COMMAND nexus_input_tests)
set_tests_properties(window_input PROPERTIES LABELS functional TIMEOUT 60)

# A terminal tab with a real shell on a pseudoterminal: a functional test (a process, real time). Its working directory
# is its own, under the build tree.
add_executable(nexus_terminal_tests test_terminal.cpp)
target_compile_options(nexus_terminal_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_terminal_tests PRIVATE nexus_terminals)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/terminal_work)
add_test(NAME window_terminal COMMAND nexus_terminal_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/terminal_work)
set_tests_properties(window_terminal PROPERTIES LABELS functional TIMEOUT 60)
