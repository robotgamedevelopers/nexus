# iOS and iPadOS: the client alone, one universal app for iPhone and iPad (docs/ios.md). Included from CMakeLists.txt
# when CMAKE_SYSTEM_NAME is iOS, before the options, so that it sets their defaults here; its own targets are made
# once CMakeLists.txt has made the libraries they use (cmake_language DEFER, at the end of the top directory).
#
#   cmake -S . -B build-ios-sim -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release            (the simulator, where tests run)
#   cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release            (devices: builds, signed ad hoc)
#
# What iOS cannot have is left out by design, never built to fail at run time: the system starts no processes for an
# app, so there is no nexus-server here, no terminal tab (a local shell), no sprint tab (its board's address comes from
# running nova-sprint), and nothing over Prax's lib/process. The window's logic that needs none of these (its home and
# its saved preferences, src/client_home.h and src/tab_order.h) is built on its own (nexus_ios_logic), and
# src/build_parts.h says which parts a build holds (NEXUS_PROCESSES). The link to nexus-server is the desktop's (mosh's
# transport after `nexus-server new` over ssh), with the ssh inside the app (src/link/inapp_ssh.h) and the libraries the
# desktop takes from the machine built here (cmake/ios_link.cmake).

set(NEXUS_IOS ON)
if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "nexus: iOS builds are arm64 only (devices and Apple Silicon's simulator); configure with "
        "-DCMAKE_OSX_ARCHITECTURES=arm64")
endif()
if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(NEXUS_IOS_SIMULATOR ON)
    set(NEXUS_RUST_TARGET aarch64-apple-ios-sim)
else()
    set(NEXUS_IOS_SIMULATOR OFF)
    set(NEXUS_RUST_TARGET aarch64-apple-ios)
endif()
set(NEXUS_RUST_TARGET_ENV "IPHONEOS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")

# The desktop window (the overlay over AppKit, its terminals and sprint tab), its logic over lib/process, and the
# desktop link and nexus-server (NEXUS_LINK: the link over /usr/bin/ssh, with the server) are not iOS's. The defaults are set here, before CMakeLists.txt's options read them; turning one on fails.
set(NEXUS_WINDOW OFF CACHE BOOL "Build the window (the app, its view, terminals and sprint tab, and their tests)")
set(NEXUS_WINDOW_LOGIC OFF CACHE BOOL "Build the window's logic over Prax's lib/process, and what uses it")
set(NEXUS_LINK OFF CACHE BOOL "Build nexus-server and the window's --server over mosh's transport (needs protobuf)")
foreach(desktop NEXUS_WINDOW NEXUS_WINDOW_LOGIC NEXUS_LINK NEXUS_TAILSCALE)
    if(${desktop})
        message(FATAL_ERROR "nexus: ${desktop} is not built for iOS (cmake/ios.cmake says why); configure without it")
    endif()
endforeach()

# Starlark (cmake/policy.cmake) is Rust, built for the iOS target: its standard library must be installed for the
# toolchain that builds it. rustup's is chosen where rustup is, since a standalone toolchain (such as the fleet's in
# ~/sdk) holds the host's standard library only.
if(NOT NEXUS_CARGO)
    find_program(nexus_rustup rustup HINTS "$ENV{HOME}/.cargo/bin")
    if(nexus_rustup)
        execute_process(COMMAND "${nexus_rustup}" which cargo OUTPUT_VARIABLE nexus_rustup_cargo
            OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE nexus_rustup_result ERROR_QUIET)
        if(nexus_rustup_result EQUAL 0 AND EXISTS "${nexus_rustup_cargo}")
            set(NEXUS_CARGO "${nexus_rustup_cargo}" CACHE FILEPATH "Cargo, whose toolchain has the iOS target")
        endif()
    endif()
endif()
if(NOT NEXUS_CARGO)
    message(FATAL_ERROR "nexus: no Cargo for the iOS build. Install rustup (https://rustup.rs), then "
        "`rustup target add aarch64-apple-ios aarch64-apple-ios-sim`; or pass -DNEXUS_CARGO=<cargo> of a toolchain "
        "that has ${NEXUS_RUST_TARGET}")
endif()
cmake_path(GET NEXUS_CARGO PARENT_PATH nexus_cargo_bin)
find_program(NEXUS_RUSTC rustc HINTS "${nexus_cargo_bin}" NO_DEFAULT_PATH)
execute_process(COMMAND "${NEXUS_RUSTC}" --print target-libdir --target ${NEXUS_RUST_TARGET}
    OUTPUT_VARIABLE nexus_rust_libdir OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE nexus_rust_result ERROR_QUIET)
file(GLOB nexus_rust_std "${nexus_rust_libdir}/libstd-*.rlib")
if(NOT nexus_rust_result EQUAL 0 OR NOT nexus_rust_std)
    message(FATAL_ERROR "nexus: ${NEXUS_RUSTC} has no standard library for ${NEXUS_RUST_TARGET}, which the iOS build "
        "of Starlark (policy/native) needs. With rustup: `rustup target add ${NEXUS_RUST_TARGET}`")
endif()

# Tests built for the simulator run on a simulator device, through Prax's own runner (tools/build/simulator.py, which
# boots an available iOS device unless PR_SIMULATOR_DEVICE names one, by name or UDID). Its environment reaches the
# program only for PR_ and MTL_ variables, so nexus's tests take what they need as arguments. A device build's tests
# are built, not run.
if(NEXUS_IOS_SIMULATOR)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${Python3_EXECUTABLE};${prax_SOURCE_DIR}/tools/build/simulator.py;--platform;ios")
endif()

# The link's libraries (protobuf's lite runtime, mbedTLS, libssh2), then mosh's transport over them. Made here, not in
# the deferred function below: they are subdirectories, which deferred code cannot add.
include(${CMAKE_CURRENT_LIST_DIR}/ios_link.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/mosh.cmake)

# The commit the app is built from, in its Info.plist (NexusCommit), so that what a device runs can be read off it
# (plutil -extract NexusCommit raw nexus.app/Info.plist). Read at configure: configure again after a new commit.
execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD OUTPUT_VARIABLE NEXUS_IOS_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE nexus_git_result ERROR_QUIET)
if(NOT nexus_git_result EQUAL 0 OR NOT NEXUS_IOS_COMMIT)
    message(FATAL_ERROR "nexus: the iOS build names the commit it is built from, and git rev-parse HEAD failed in "
        "${PROJECT_SOURCE_DIR}")
endif()

function(nexus_ios_targets)
    # The window's logic that starts no process: its home and config (client_home.h) and the preferences it keeps
    # (tab_order.h). The same sources as the desktop's nexus_window_logic, less command.cpp, remote.cpp and sprint.cpp.
    add_library(nexus_ios_logic STATIC src/tab_order.cpp src/client_home.cpp)
    target_include_directories(nexus_ios_logic PUBLIC src)
    target_compile_options(nexus_ios_logic PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_ios_logic PUBLIC nexus_config nexus_model nexus_accounts prax::json prax::core_cpp)

    # The window's tree: the same sources as the desktop's nexus_view.
    add_library(nexus_ios_view STATIC src/view.cpp src/chat_markdown.cpp src/chat_selection.cpp src/window_app.cpp
        src/native_tabs.cpp src/font_prefetch.cpp)
    target_include_directories(nexus_ios_view PUBLIC src)
    target_compile_options(nexus_ios_view PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_ios_view PUBLIC nexus_policy nexus_ios_logic nexus_sync nexus_hitch
        prax::markdown prax::studio_kit prax::ui prax::ui_cpp prax::layout prax::font prax::text prax::draw prax::app
        prax::core_cpp)

    # What the window does around the view each frame (frame_host.h), without terminals, board or uploads
    # (NEXUS_PROCESSES is 0 here, platform.h), and its offscreen frame (offscreen.h).
    add_library(nexus_ios_frame STATIC src/frame_host.cpp src/url_opener.cpp src/offscreen.cpp src/state_window.cpp)
    target_include_directories(nexus_ios_frame PUBLIC src)
    target_compile_options(nexus_ios_frame PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_ios_frame PUBLIC nexus_ios_view nexus_sync nexus_hitch nexus_signin_gate
        prax::image_select prax::compress_select prax::gpu_select prax::draw_select Threads::Threads)

    # The link to nexus-server (src/link/client.h), the desktop's sources, with the in-app ssh in place of
    # /usr/bin/ssh. Apart from the window, so that another front (a watch's, say) links it alone.
    add_library(nexus_ios_link STATIC src/link/mosh_link.cpp src/link/bootstrap.cpp src/link/client.cpp
        src/link/inapp_ssh.cpp)
    target_include_directories(nexus_ios_link PUBLIC src)
    target_compile_options(nexus_ios_link PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_ios_link PUBLIC nexus_sync mosh_transport Threads::Threads PRIVATE nexus_libssh2
        mbedcrypto)

    # The app: one window filling its scene, on iPhone and iPad (cmake/ios_info.plist.in), signed ad hoc as the
    # simulator requires. A device needs the developer's signature (NEXUS_IOS_TEAM below, docs/ios.md).
    add_executable(nexus_ios MACOSX_BUNDLE src/main_ios.cpp src/platform_ios.mm)
    set_source_files_properties(src/platform_ios.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_options(nexus_ios PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_ios PRIVATE nexus_ios_frame nexus_ios_view nexus_ios_link prax::studio_app prax::app_select
        prax::gpu_select prax::draw_select prax::image_select prax::compress_select "-framework UIKit")
    set_target_properties(nexus_ios PROPERTIES
        OUTPUT_NAME nexus
        MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/cmake/ios_info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.robotgamedevelopers.nexus
        MACOSX_BUNDLE_BUNDLE_NAME nexus
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    add_custom_command(TARGET nexus_ios POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:nexus_ios> VERBATIM)

    # The headless frame of a state file, `nexus --screenshot --state` on iOS: the same offscreen render, run on a
    # simulator device by the test below.
    add_executable(nexus_ios_shot tests/ios/state_shot.cpp)
    target_compile_options(nexus_ios_shot PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_ios_shot PRIVATE nexus_ios_frame prax::png)
    include(${PROJECT_SOURCE_DIR}/tests/ios/tests.cmake)
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL nexus_ios_targets)
