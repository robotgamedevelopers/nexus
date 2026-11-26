# The iOS build's own tests, included by cmake/ios.cmake once its targets are made (a deferred call, which may not add a
# directory). A simulator build runs them on a simulator device through ctest's
# emulator, Prax's runner; a device build builds them only.
#
# window_ios_state_shot_<device>: `nexus --screenshot --state` on iOS (tests/ios/state_shot.cpp), the window's frame of
# the window tests' fixture state at an iPhone's and an iPad's size, compared with the golden image beside this file.
# A functional test: text is set in the system's fonts. The golden images are what this test wrote on the iOS 27.2
# simulator, checked by eye; after a change that is meant to move the frame, write them again with
#   cmake --build <build> --target nexus_ios_goldens
# and look at each before committing it.
#
# window_ios_state_shot_planted: the reversed witness, which must fail: the iPhone's frame of the state as the window
# shows it once it has lost the server, against the golden of the state as it shows once heard.
#
# window_ios_status_no_tabs: the window before the server has sent a tab, with the link's line saying why it failed
# (policy/rules/client/window.star shows the status panel when there is no tab), at an iPad's size, against its golden;
# its reversed witness, window_ios_status_no_tabs_planted, is the same window with no line, which must not match.
#
# Every shot is at the text scale an iOS window starts at (src/style.h kDefaultTextSteps, smaller than the desktop's);
# window_ios_text_scale_planted, the reversed witness, is the iPad's shot at the desktop's scale, which must not match.
#
# window_ios_server_dialog: the window at first launch, with no server kept (src/server_choice.h), asking for one,
# with a host typed and no user, and the reason under the field; its reversed witness,
# window_ios_server_dialog_planted, is the same window with no dialog, which must not match.

set(state ${PROJECT_SOURCE_DIR}/tests/window/fixtures/busy.state)
set(golden ${PROJECT_SOURCE_DIR}/tests/ios/golden)
set(work ${CMAKE_BINARY_DIR}/tests/ios/shots)
file(MAKE_DIRECTORY ${work})

# At the sizes in points and pixel ratios of an iPhone 17 Pro and a 13-inch iPad Pro, in portrait.
function(nexus_ios_shot_test device width height ratio)
    add_test(NAME window_ios_state_shot_${device}
        COMMAND nexus_ios_shot ${state} ${work}/${device}.png ${width} ${height} ${ratio}
            --compare ${golden}/${device}.png)
    set_tests_properties(window_ios_state_shot_${device} PROPERTIES LABELS "functional;ios" TIMEOUT 60)
endfunction()
nexus_ios_shot_test(iphone 402 874 3)
nexus_ios_shot_test(ipad 1032 1376 2)

add_test(NAME window_ios_state_shot_planted
    COMMAND nexus_ios_shot ${state} ${work}/planted.png 402 874 3 --lost --compare ${golden}/iphone.png)
set_tests_properties(window_ios_state_shot_planted PROPERTIES LABELS "functional;ios" TIMEOUT 60 WILL_FAIL TRUE)

set(status_line "me@mac: the server did not take this device's key")
add_test(NAME window_ios_status_no_tabs
    COMMAND nexus_ios_shot - ${work}/status.png 1032 1376 2 --status ${status_line} --compare ${golden}/status.png)
set_tests_properties(window_ios_status_no_tabs PROPERTIES LABELS "functional;ios" TIMEOUT 60)
add_test(NAME window_ios_status_no_tabs_planted
    COMMAND nexus_ios_shot - ${work}/status-planted.png 1032 1376 2 --compare ${golden}/status.png)
set_tests_properties(window_ios_status_no_tabs_planted PROPERTIES LABELS "functional;ios" TIMEOUT 60 WILL_FAIL TRUE)

add_test(NAME window_ios_text_scale_planted
    COMMAND nexus_ios_shot ${state} ${work}/scale-planted.png 1032 1376 2 --text-steps 0 --compare ${golden}/ipad.png)
set_tests_properties(window_ios_text_scale_planted PROPERTIES LABELS "functional;ios" TIMEOUT 60 WILL_FAIL TRUE)

add_test(NAME window_ios_server_dialog
    COMMAND nexus_ios_shot - ${work}/server.png 1032 1376 2 --ask-server mac --compare ${golden}/server.png)
set_tests_properties(window_ios_server_dialog PROPERTIES LABELS "functional;ios" TIMEOUT 60)
add_test(NAME window_ios_server_dialog_planted
    COMMAND nexus_ios_shot - ${work}/server-planted.png 1032 1376 2 --compare ${golden}/server.png)
set_tests_properties(window_ios_server_dialog_planted PROPERTIES LABELS "functional;ios" TIMEOUT 60 WILL_FAIL TRUE)

# Writes the golden images again (see above): each shot without --compare, straight into tests/ios/golden.
if(NEXUS_IOS_SIMULATOR)
    add_custom_target(nexus_ios_goldens
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:nexus_ios_shot> ${state} ${golden}/iphone.png 402 874 3
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:nexus_ios_shot> ${state} ${golden}/ipad.png 1032 1376 2
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:nexus_ios_shot> - ${golden}/status.png 1032 1376 2
            --status ${status_line}
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:nexus_ios_shot> - ${golden}/server.png 1032 1376 2
            --ask-server mac
        DEPENDS nexus_ios_shot
        COMMENT "Writing tests/ios/golden/iphone.png, ipad.png, status.png and server.png on the simulator; look at each before committing"
        VERBATIM)
endif()

# ios_stock_window: the window with no applied policy bundle, as on a new install, is the shipped profile's: Dashboard
# and capitalised names, the account menu centred (tests/window/stock_window_test.cpp), on a simulator.
add_executable(ios_stock_window ${PROJECT_SOURCE_DIR}/tests/window/stock_window_test.cpp)
target_compile_options(ios_stock_window PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(ios_stock_window PRIVATE nexus_ios_view)
add_test(NAME ios_stock_window COMMAND ios_stock_window)
set_tests_properties(ios_stock_window PROPERTIES LABELS "unit;ios" TIMEOUT 30)

# ios_keyboard_unit: the room above the on-screen keyboard (src/keyboard_room.h), with neither safe-area inset counted
# twice.
add_executable(ios_keyboard_unit ${PROJECT_SOURCE_DIR}/tests/ios/keyboard_unit.cpp)
target_compile_options(ios_keyboard_unit PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(ios_keyboard_unit PRIVATE nexus_ios_view)
add_test(NAME ios_keyboard_unit COMMAND ios_keyboard_unit)
set_tests_properties(ios_keyboard_unit PROPERTIES LABELS "unit;ios" TIMEOUT 30)

# ios_ssh_unit: the in-app ssh's parts that need no connection (tests/ios/ssh_unit.cpp), in a directory of its own.
add_executable(ios_ssh_unit ${PROJECT_SOURCE_DIR}/tests/ios/ssh_unit.cpp)
target_compile_options(ios_ssh_unit PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(ios_ssh_unit PRIVATE nexus_ios_link)
add_test(NAME ios_ssh_unit COMMAND ios_ssh_unit ${CMAKE_BINARY_DIR}/tests/ios/ssh)
set_tests_properties(ios_ssh_unit PROPERTIES LABELS "unit;ios" TIMEOUT 30)

# ios_ssh_gate: tools/nexus-ios-gate, the forced command of a device's ssh key on the server's machine, runs the link's
# own lines and refuses anything else (tests/ios/gate_test.sh). On the building Mac, not a simulator: the gate runs on
# the server's machine. SHELL is /bin/echo there, so nothing is run.
add_test(NAME ios_ssh_gate COMMAND sh ${PROJECT_SOURCE_DIR}/tests/ios/gate_test.sh ${PROJECT_SOURCE_DIR}/tools/nexus-ios-gate)
set_tests_properties(ios_ssh_gate PROPERTIES LABELS "unit;ios" TIMEOUT 30)

# ios_ssh_functional: the in-app ssh end to end on a simulator device (tests/ios/ssh_functional.cpp), against an sshd
# of the test's own on 127.0.0.1 and the real gate, which the fixture starts on the building Mac and stops after
# (tests/ios/sshd_fixture.sh). Built for devices too; run on the simulator.
set(sshd_dir ${CMAKE_BINARY_DIR}/tests/ios/sshd)
add_executable(ios_ssh_functional ${PROJECT_SOURCE_DIR}/tests/ios/ssh_functional.cpp)
target_compile_options(ios_ssh_functional PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(ios_ssh_functional PRIVATE nexus_ios_link)
add_test(NAME ios_sshd_start COMMAND sh ${PROJECT_SOURCE_DIR}/tests/ios/sshd_fixture.sh start ${sshd_dir}
    ${PROJECT_SOURCE_DIR}/tools/nexus-ios-gate)
add_test(NAME ios_sshd_stop COMMAND sh ${PROJECT_SOURCE_DIR}/tests/ios/sshd_fixture.sh stop ${sshd_dir})
add_test(NAME ios_ssh_functional COMMAND ios_ssh_functional ${sshd_dir})
set_tests_properties(ios_sshd_start PROPERTIES LABELS "functional;ios" TIMEOUT 30 FIXTURES_SETUP ios_sshd)
set_tests_properties(ios_sshd_stop PROPERTIES LABELS "functional;ios" TIMEOUT 30 FIXTURES_CLEANUP ios_sshd)
set_tests_properties(ios_ssh_functional PROPERTIES LABELS "functional;ios" TIMEOUT 60 FIXTURES_REQUIRED ios_sshd)
