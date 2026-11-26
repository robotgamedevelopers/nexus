# nexus on Apple Watch (docs/watch.md): a thin client of nexus-server through the paired iPhone, voice first both ways.
#
#   core/   the voice turn, the AI-name matcher and the relay's two ends, logic only (no clock, no I/O), with a C
#           interface for Swift (nexus_watch.h). Built everywhere, unit tested and bound to its TLA+ models (models/).
#   app/    the watchOS app, SwiftUI: built by cmake/watchos.cmake when CMAKE_SYSTEM_NAME is watchOS.
#   phone/  the iPhone's end: the WatchConnectivity carrier and the recognizer (Transcriber.swift).
#   host/   the phone's relay over nexus's own link (nexus_relay.h), and nexus-watch-relay, which runs it on a Mac to
#           stand in for the iPhone in the simulator's end-to-end test: with the link (NEXUS_LINK) on macOS, under Ninja
#           (CMake builds Swift with Ninja and Xcode only).
#
# Standalone, for the watch's own CI job (no Prax, no link): cmake -S watch -B build-watch

if(NOT DEFINED PROJECT_NAME)
    cmake_minimum_required(VERSION 3.28)
    project(nexus_watch LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(NEXUS_WARNINGS -Wall -Wextra -Wpedantic -Werror)
    enable_testing()
endif()

# The name matcher is the iPhone's (src/voice/name_match.h), shared, so the two route the same words alike.
set(NEXUS_WATCH_SRC "${CMAKE_CURRENT_LIST_DIR}/../src")
add_library(nexus_watch_core STATIC core/ai_match.cpp core/voice_turn.cpp core/relay.cpp core/screen.cpp core/nexus_watch.cpp
    ${NEXUS_WATCH_SRC}/voice/name_match.cpp)
target_include_directories(nexus_watch_core PUBLIC core PRIVATE ${NEXUS_WATCH_SRC})
target_compile_features(nexus_watch_core PUBLIC cxx_std_20)
target_compile_options(nexus_watch_core PRIVATE ${NEXUS_WARNINGS})

if(CMAKE_SYSTEM_NAME STREQUAL "watchOS")
    return()
endif()

add_subdirectory(tests)

if(TARGET nexus_link AND APPLE AND CMAKE_GENERATOR MATCHES "Ninja")
    enable_language(Swift)
    # A shared library, linked by clang++: the link's own libraries carry -Wl flags that swiftc refuses, and a shared
    # library keeps them to itself. Swift sees only the C interface, nexus_relay.h.
    add_library(nexus_watch_relay SHARED host/relay_host.cpp)
    target_include_directories(nexus_watch_relay PUBLIC host)
    target_compile_options(nexus_watch_relay PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_watch_relay PRIVATE nexus_link nexus_watch_core)
    add_executable(nexus_watch_relay_host host/main.swift phone/Transcriber.swift)
    set_target_properties(nexus_watch_relay_host PROPERTIES OUTPUT_NAME nexus-watch-relay)
    target_compile_options(nexus_watch_relay_host PRIVATE -warnings-as-errors
        -import-objc-header "${CMAKE_CURRENT_SOURCE_DIR}/host/nexus_relay.h")
    target_link_libraries(nexus_watch_relay_host PRIVATE nexus_watch_relay)

    # The speech test (functional): spoken recordings through the recognizer and the name matcher (tests/speech.cmake).
    add_executable(watch_match tests/match_tool.cpp)
    target_link_libraries(watch_match PRIVATE nexus_watch_core)
    target_compile_options(watch_match PRIVATE ${NEXUS_WARNINGS})
    foreach(plant 0 1)
        set(name watch_speech)
        if(plant)
            set(name watch_speech_planted)
        endif()
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} "-DRELAY=$<TARGET_FILE:nexus_watch_relay_host>"
            "-DMATCH=$<TARGET_FILE:watch_match>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/speech/${name}" "-DPLANT=${plant}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/speech.cmake")
        set_tests_properties(${name} PROPERTIES LABELS functional TIMEOUT 300 WILL_FAIL ${plant})
    endforeach()
endif()
