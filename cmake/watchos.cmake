# Apple Watch: the watch app alone (docs/watch.md), outside Prax and the desktop build. Included from CMakeLists.txt
# when CMAKE_SYSTEM_NAME is watchOS, which then builds nothing else. SwiftUI over the watch's C++ core (watch/core,
# through its C interface nexus_watch.h). CMake builds Swift with the Ninja and Xcode generators only.
#
#   cmake -S . -B build-watch-sim -G Ninja -DCMAKE_SYSTEM_NAME=watchOS -DCMAKE_OSX_SYSROOT=watchsimulator \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-watch-sim        (build-watch-sim/watch/nexus.app, signed ad hoc for the simulator)
#
# A device build (-DCMAKE_OSX_SYSROOT=watchos -DCMAKE_OSX_ARCHITECTURES="arm64_32;arm64") is signed with the team
# given as NEXUS_WATCH_TEAM (an identity in the keychain), or ad hoc when none is given (which no watch installs).

if(NOT CMAKE_GENERATOR MATCHES "Ninja|Xcode")
    message(FATAL_ERROR "nexus: the watch app is Swift, which CMake builds with -G Ninja or -G Xcode only")
endif()
# Swift needs the target spelled out: CMake gives it the SDK, and swiftc would otherwise target the host.
if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(nexus_watch_triple_suffix "-simulator")
endif()
list(GET CMAKE_OSX_ARCHITECTURES 0 nexus_watch_arch)
set(CMAKE_Swift_COMPILER_TARGET "${nexus_watch_arch}-apple-watchos${CMAKE_OSX_DEPLOYMENT_TARGET}${nexus_watch_triple_suffix}")
# /usr/bin/swiftc is xcrun's shim, which sets SDKROOT to the macOS SDK when it is unset; the clang that swiftc links
# with then warns that the macOS sysroot does not fit the watch. The toolchain's own swiftc takes the SDK from -sdk.
if(NOT CMAKE_Swift_COMPILER)
    execute_process(COMMAND xcrun -f swiftc OUTPUT_VARIABLE nexus_swiftc OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE nexus_swiftc_failed)
    if(nexus_swiftc_failed)
        message(FATAL_ERROR "nexus: xcrun cannot find swiftc; install Xcode and run xcode-select")
    endif()
    set(CMAKE_Swift_COMPILER "${nexus_swiftc}")
endif()
enable_language(Swift)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(NEXUS_WARNINGS -Wall -Wextra -Wpedantic -Werror)
add_subdirectory(watch)

if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(NEXUS_WATCH_PLATFORM WatchSimulator)
else()
    set(NEXUS_WATCH_PLATFORM WatchOS)
endif()
string(TOLOWER "${NEXUS_WATCH_PLATFORM}" NEXUS_WATCH_PLATFORM_LOWER)
set(NEXUS_WATCH_TEAM "" CACHE STRING "Code-signing identity for a device build; empty signs ad hoc")

set(app "${PROJECT_SOURCE_DIR}/watch/app")
add_executable(nexus_watch_app MACOSX_BUNDLE ${app}/NexusWatchApp.swift ${app}/Client.swift ${app}/Voice.swift
    ${app}/Carrier.swift ${app}/Lock.swift ${app}/AccountList.swift)
set_target_properties(nexus_watch_app PROPERTIES
    OUTPUT_NAME nexus
    MACOSX_BUNDLE_INFO_PLIST "${app}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER com.robotgamedevelopers.nexus.watchkitapp
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/watch")
target_compile_options(nexus_watch_app PRIVATE -warnings-as-errors
    -import-objc-header "${PROJECT_SOURCE_DIR}/watch/core/nexus_watch.h")
target_link_libraries(nexus_watch_app PRIVATE nexus_watch_core)
# The simulator's test options (--relay, --speak-file, --voice-to, --log, --drive) and the driven wrist exist only in a
# Debug build for the simulator; Release, and every build for a watch, has none of them (watch/app/Client.swift).
if(NEXUS_WATCH_PLATFORM STREQUAL "WatchSimulator")
    target_compile_options(nexus_watch_app PRIVATE "$<$<CONFIG:Debug>:-DNEXUS_WATCH_TEST_OPTIONS>")
endif()
if(NEXUS_WATCH_TEAM)
    set(sign "${NEXUS_WATCH_TEAM}")
else()
    set(sign "-")
endif()
add_custom_command(TARGET nexus_watch_app POST_BUILD
    COMMAND /usr/bin/codesign --force --sign "${sign}" --timestamp=none "$<TARGET_BUNDLE_DIR:nexus_watch_app>"
    COMMENT "Signing nexus.app (${sign})")
