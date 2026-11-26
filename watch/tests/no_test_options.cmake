# The app's binary carries none of the simulator's test options (watch/app/Client.swift, Options): a Release build, or
# any build for a watch, must not read --relay, --speak-file, --voice-to, --log, --drive or --canned-phone, nor drive
# the wrist.
#   cmake -DAPP=<nexus.app/nexus> [-DEXPECT=present] -P watch/tests/no_test_options.cmake
# EXPECT=present is the planted twin: run on a Debug build for the simulator, which has them, it must find them.

if(NOT DEFINED APP OR NOT EXISTS "${APP}")
    message(FATAL_ERROR "no_test_options.cmake: no app binary at '${APP}'")
endif()
file(STRINGS "${APP}" found REGEX "--(relay|speak-file|voice-to|log|drive|canned-phone)$|^wait:$")
if(DEFINED EXPECT AND EXPECT STREQUAL "present")
    if(NOT found)
        message(FATAL_ERROR "${APP}: no test options found, where this build must have them (the check sees nothing)")
    endif()
    message(STATUS "${APP}: has the test options, as a Debug build for the simulator must: ${found}")
    return()
endif()
if(found)
    message(FATAL_ERROR "${APP} carries the simulator's test options (${found}): they must exist only in a Debug build "
        "for the simulator (NEXUS_WATCH_TEST_OPTIONS, cmake/watchos.cmake)")
endif()
message(STATUS "${APP}: no test options")
