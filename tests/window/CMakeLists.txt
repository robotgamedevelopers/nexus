# The tests of the window's logic (NEXUS_WINDOW_LOGIC in CMakeLists.txt: it starts processes through Prax's
# lib/process).
if(NEXUS_WINDOW_LOGIC)
    # The window's unit tests: pure logic only (no OS, no clock, no window, no network).
    add_executable(nexus_window_tests test_window.cpp)
    target_compile_options(nexus_window_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_window_tests PRIVATE nexus_window_logic)
    add_test(NAME window_unit COMMAND nexus_window_tests)
    set_tests_properties(window_unit PROPERTIES LABELS unit)

    # A terminal tab's label in ssh (src/remote.h): the destination's parsing and matching, the machine list through a fake
    # command runner, and the label across sessions. Pure logic.
    add_executable(nexus_remote_tests test_remote.cpp)
    target_compile_options(nexus_remote_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_remote_tests PRIVATE nexus_window_logic)
    add_test(NAME remote_unit COMMAND nexus_remote_tests)
    set_tests_properties(remote_unit PROPERTIES LABELS unit)

    # The sprint tab's logic (src/sprint.h): the board's address from the sprint's record through a fake command runner,
    # event routing, and the page's prelude and style as text. Pure logic.
    add_executable(nexus_sprint_tests test_sprint.cpp)
    target_compile_options(nexus_sprint_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_sprint_tests PRIVATE nexus_window_logic)
    add_test(NAME sprint_unit COMMAND nexus_sprint_tests)
    set_tests_properties(sprint_unit PROPERTIES LABELS unit)

    # The window's home and config (src/client_home.h): the home from HOME, and the config's parsing. Pure logic.
    add_executable(nexus_client_home_tests test_client_home.cpp)
    target_compile_options(nexus_client_home_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_client_home_tests PRIVATE nexus_window_logic)
    add_test(NAME client_home_unit COMMAND nexus_client_home_tests)
    set_tests_properties(client_home_unit PROPERTIES LABELS unit)

    # Running a command (src/friends.h runCommand): real child processes and a real clock.
    add_executable(nexus_command_tests test_command.cpp)
    target_compile_options(nexus_command_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_command_tests PRIVATE nexus_window_logic)
    add_test(NAME window_command COMMAND nexus_command_tests)
    set_tests_properties(window_command PROPERTIES LABELS functional TIMEOUT 30)

    # The tab order's file store: a real file, in a directory of the test's own; and the window's home, ~/.nexus/client,
    # under a HOME inside that directory.
    add_executable(nexus_tab_order_file_tests test_tab_order_file.cpp)
    target_compile_options(nexus_tab_order_file_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_tab_order_file_tests PRIVATE nexus_window_logic)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tab_order_file)
    add_test(NAME tab_order_file COMMAND nexus_tab_order_file_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tab_order_file)
    set_tests_properties(tab_order_file PROPERTIES LABELS functional TIMEOUT 30)
endif()

# The hitch detector (src/hitch.h): the budget, phases, the ring and the log on a fake clock. Pure logic.
add_executable(nexus_hitch_tests test_hitch.cpp)
target_compile_options(nexus_hitch_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_hitch_tests PRIVATE nexus_hitch)
add_test(NAME hitch_unit COMMAND nexus_hitch_tests)
set_tests_properties(hitch_unit PROPERTIES LABELS unit)

# The sprint board's scrolling (src/scroll_glide.h): the curves, a tap against a hold, repeats, release, focus loss, the
# ends. Time is an argument.
add_executable(nexus_scroll_glide_tests test_scroll_glide.cpp)
target_compile_options(nexus_scroll_glide_tests PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_scroll_glide_tests PRIVATE nexus_scroll)
add_test(NAME scroll_glide_unit COMMAND nexus_scroll_glide_tests)
set_tests_properties(scroll_glide_unit PROPERTIES LABELS unit)

# The tests that draw or set text: on when the window is built (NEXUS_WINDOW in CMakeLists.txt).
if(NEXUS_WINDOW)
    # The sprint tab's page (src/board.h) headless over Prax's HTML stack: a fixture board served from memory on Prax's null
    # network (no socket), QuickJS scripts and the system's fonts, with made-up clocks. A functional test, in a working
    # directory of its own.
    add_executable(nexus_board_tests test_board.cpp)
    target_compile_options(nexus_board_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_board_tests PRIVATE nexus_board nexus_view)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/board_work)
    add_test(NAME sprint_board COMMAND nexus_board_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/board_work)
    set_tests_properties(sprint_board PROPERTIES LABELS functional TIMEOUT 60)

    # The window's input, headless: the view over a Prax UI context with no window and no GPU, fed key events. The clock is
    # made up, but text is set in the system's fonts, an outside service: so a functional test, not a unit test.
    add_executable(nexus_input_tests test_input.cpp)
    target_compile_options(nexus_input_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_input_tests PRIVATE nexus_view)
    add_test(NAME window_input COMMAND nexus_input_tests)
    set_tests_properties(window_input PROPERTIES LABELS functional TIMEOUT 60)

    # A terminal tab with a real shell on a pseudoterminal: a functional test (a process, real time). Its working directory
    # is its own, under the build tree.
    # A stand-in ssh, named ssh in a directory of its own, connects nowhere (fake_ssh.cpp): the test puts it first on the
    # PATH.
    add_executable(nexus_fake_ssh fake_ssh.cpp)
    target_compile_options(nexus_fake_ssh PRIVATE ${NEXUS_WARNINGS})
    set_target_properties(nexus_fake_ssh PROPERTIES OUTPUT_NAME ssh RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake_ssh)
    add_executable(nexus_terminal_tests test_terminal.cpp)
    target_compile_options(nexus_terminal_tests PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_terminal_tests PRIVATE nexus_terminals)
    add_dependencies(nexus_terminal_tests nexus_fake_ssh)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/terminal_work)
    add_test(NAME window_terminal COMMAND nexus_terminal_tests $<TARGET_FILE_DIR:nexus_fake_ssh>
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/terminal_work)
    set_tests_properties(window_terminal PROPERTIES LABELS functional TIMEOUT 60)

    # The frame budget (test_frames.cpp): the window's real frame code (src/frame_host.h) driven against slow fakes, each
    # frame's main-thread work measured by the hitch detector on the real clock against 8.3 ms (120 Hz). Real shells,
    # real time and the system's fonts: a functional test, in a working directory of its own. slow_ssh stands in for an
    # ssh whose host never answers.
    if(NEXUS_LINK)
        add_executable(nexus_slow_ssh slow_ssh.cpp)
        target_compile_options(nexus_slow_ssh PRIVATE ${NEXUS_WARNINGS})
        add_executable(nexus_frame_tests test_frames.cpp)
        target_compile_options(nexus_frame_tests PRIVATE ${NEXUS_WARNINGS})
        target_link_libraries(nexus_frame_tests PRIVATE nexus_frame nexus_link)
        add_dependencies(nexus_frame_tests nexus_slow_ssh)
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/frame_work)
        add_test(NAME window_frame_budget COMMAND nexus_frame_tests $<TARGET_FILE:nexus_slow_ssh>
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/frame_work)
        set_tests_properties(window_frame_budget PROPERTIES LABELS functional TIMEOUT 90)
        # The proof that a real hitch fails every run: 10 ms of busy work on one frame, in a case whose frame count is
        # fixed (matched by frame number) and in one that waits on real time (matched by its place among the events).
        add_test(NAME window_frame_budget_injected COMMAND nexus_frame_tests $<TARGET_FILE:nexus_slow_ssh>
            --inject typing_over_a_long_history 100 typing_over_a_long_history
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/frame_work)
        set_tests_properties(window_frame_budget_injected PROPERTIES LABELS functional TIMEOUT 60
            PASS_REGULAR_EXPRESSION "HITCH in every run: frame 100, phase keep")
        add_test(NAME window_frame_budget_injected_paced COMMAND nexus_frame_tests $<TARGET_FILE:nexus_slow_ssh>
            --inject text_scale 20 text_scale
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/frame_work)
        set_tests_properties(window_frame_budget_injected_paced PROPERTIES LABELS functional TIMEOUT 60
            PASS_REGULAR_EXPRESSION "HITCH in every run: frame 20, phase keep")
    endif()

    # `nexus --state` against a fixture state (fixtures/busy.state, written by tests/server's friends_functional_window
    # from the fake driver), with HOME an empty directory: it draws from the file alone (state_screenshot.cmake).
    if(NEXUS_LINK AND TARGET nexus)
        add_test(NAME window_state_screenshot COMMAND ${CMAKE_COMMAND} -DNEXUS=$<TARGET_FILE:nexus>
            -DSTATE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/busy.state -DWORK=${CMAKE_CURRENT_BINARY_DIR}/state_screenshot
            -P ${CMAKE_CURRENT_SOURCE_DIR}/state_screenshot.cmake)
        set_tests_properties(window_state_screenshot PROPERTIES LABELS functional TIMEOUT 60)
    endif()
endif()
