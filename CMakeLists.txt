# nexus: one person and several AIs in one window. The window runs on macOS, and on 64-bit Windows over Prax's Win32
# backend (NEXUS_WINDOW below), where a native build holds the window's code alone and its link runs inside WSL2
# (NEXUS_SERVER_PARTS, NEXUS_LINK below); on Linux the parts without a window build and test.
cmake_minimum_required(VERSION 3.28)

# Prax runs on macOS 26 or newer; set before project(), which reads it.
set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "Oldest macOS the build runs on")

# Prax compiles with clang and links with lld on Linux, not gcc (below), so on Linux a configure that names no compiler
# takes clang where clang and lld are installed, rather than the system's cc and c++, which are gcc's on most
# distributions.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(NEXUS_CLANGXX NAMES clang++)
    find_program(NEXUS_CLANG NAMES clang)
    find_program(NEXUS_LLD NAMES ld.lld)
    if(NEXUS_CLANGXX AND NEXUS_CLANG AND NEXUS_LLD AND NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
        set(CMAKE_C_COMPILER "${NEXUS_CLANG}")
        set(CMAKE_CXX_COMPILER "${NEXUS_CLANGXX}")
    endif()
endif()

project(nexus VERSION 0.2.4 LANGUAGES C CXX)
# Apple Watch builds the watch app alone, without Prax or the desktop's parts (cmake/watchos.cmake, docs/watch.md).
if(CMAKE_SYSTEM_NAME STREQUAL "watchOS")
    include(cmake/watchos.cmake)
    return()
endif()
# What each test runs, recorded from here on, so the pull request tier builds its own targets alone (tier_pr_build,
# cmake/tiers.cmake). Before the first add_test.
include(cmake/test_targets.cmake)
# The app's platform layer (src/platform_mac.mm) is Objective-C++.
if(APPLE)
    enable_language(OBJCXX)
endif()
# Windows is 64-bit only: there is no 32-bit build.
if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "nexus: Windows builds are 64-bit only; configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

# Prax builds with clang (and MSVC on Windows), not gcc: its headers use clang's builtins (pr/type_traits.h,
# __is_trivially_destructible), so gcc fails in Prax's first file. A gcc configure still builds the targets without
# Prax (the sandbox's programs, nexus_sandbox_tools, as CI's claude-driver-wall-linux does on GitHub's runners), so
# this says so once rather than refusing.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(WARNING "nexus: Prax, which nexus builds on, compiles with clang, not gcc: with gcc only the targets "
        "without Prax build (nexus_sandbox_tools). Configure with CC=clang CXX=clang++ (on Ubuntu: apt install clang "
        "lld) for nexus.")
endif()

# Clang on Linux links with lld unless the configure names a linker: Prax builds Release with LTO
# (CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE in its CMakeLists.txt), so its static libraries hold LLVM bitcode, which
# GNU ld cannot read ("file format not recognized").
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_LINKER_TYPE)
    find_program(NEXUS_LLD NAMES ld.lld)
    if(NEXUS_LLD)
        set(CMAKE_LINKER_TYPE LLD)
    else()
        message(WARNING "nexus: clang on Linux links Prax's LTO libraries with lld, which was not found: only the "
            "targets without Prax link (nexus_sandbox_tools). Install it (on Ubuntu: apt install lld) for nexus.")
    endif()
endif()

# Prax options: leave out what this window does not use. The sprint tab's page runs its scripts in QuickJS (PR_JS).
set(PR_JS ON CACHE BOOL "" FORCE)
set(PR_CHARSET_LEGACY OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(prax
    URL https://codeload.github.com/pdoane/prax/tar.gz/87bcd16a6657ab8d93573cdf1291ba14c169f0dc
    URL_HASH SHA256=9bb8fc2473af41ae1cef8a19d61444db82125a2477ea7b95faa24adb43f25909
    # SYSTEM: Prax's headers use Clang nullability, which -Wpedantic rejects in our code otherwise.
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(prax)

# Whole-program optimisation in Release (NEXUS_LTO). Prax turns it on for its own libraries
# (CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE in its CMakeLists.txt): MSVC's /GL and /LTCG, LLVM bitcode with clang. It is
# a shipping optimisation: the nightly and shipped builds keep it, and the per-pull-request builds turn it off
# (.github/actions/windows-build and nexus-build, `lto`), where the whole-program link of each window program took most
# of the Windows job's 2 minutes (about 53 s for one, run 45856240787). Prax is never patched, so with it off nexus clears
# the property on every Prax target after Prax is added. tests/build/lto_check.cmake (build_unit_lto) holds the record
# written here to the option.
option(NEXUS_LTO "Whole-program optimisation in Release, in Prax's libraries as Prax sets it" ON)
function(nexus_prax_targets dir out)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        nexus_prax_targets("${sub}" more)
        list(APPEND targets ${more})
    endforeach()
    set(${out} "${targets}" PARENT_SCOPE)
endfunction()
nexus_prax_targets("${prax_SOURCE_DIR}" nexus_prax_all_targets)
set(nexus_prax_ipo_record "NEXUS_LTO=${NEXUS_LTO}\n")
foreach(target IN LISTS nexus_prax_all_targets)
    get_target_property(type ${target} TYPE)
    if(NOT type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
        continue()
    endif()
    if(NOT NEXUS_LTO)
        set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
            INTERPROCEDURAL_OPTIMIZATION OFF)
    endif()
    get_target_property(ipo ${target} INTERPROCEDURAL_OPTIMIZATION_RELEASE)
    string(APPEND nexus_prax_ipo_record "${target} ${ipo}\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/prax-ipo.txt" "${nexus_prax_ipo_record}")

# Prax's lib/process on Linux. At this pin its POSIX backend does not link there: process_posix.cpp (lines 165-173)
# declares `extern char** environ;` at block scope inside its own unnamed namespace, which names an `environ` nothing
# defines. Prax is pinned and never patched, so nexus works around it without touching the file: cmake/prax_environ.h,
# force-included into that one source, points the declaration at the C library's `environ` (the header says how).
# This goes away when Prax fixes the file and the pin moves past the fix: the check below refuses to configure once the
# line is gone, so the workaround is dropped then rather than left over a fixed file. macOS reads _NSGetEnviron().
if(UNIX AND NOT APPLE AND TARGET pr_process)
    set(nexus_prax_process_posix "${prax_SOURCE_DIR}/lib/process/backends/posix/src/process_posix.cpp")
    file(READ "${nexus_prax_process_posix}" nexus_prax_process_posix_text)
    string(FIND "${nexus_prax_process_posix_text}" "    extern char** environ;" nexus_prax_environ_bug)
    if(nexus_prax_environ_bug EQUAL -1)
        message(FATAL_ERROR "nexus: Prax's process_posix.cpp no longer declares `extern char** environ;` in its "
            "unnamed namespace, so the bug cmake/prax_environ.h works around is fixed at this pin: remove that header "
            "and this block from CMakeLists.txt")
    endif()
    set_source_files_properties("${nexus_prax_process_posix}" TARGET_DIRECTORY pr_process
        PROPERTIES COMPILE_OPTIONS "-include;${PROJECT_SOURCE_DIR}/cmake/prax_environ.h")
endif()

# iOS and iPadOS build the client alone (cmake/ios.cmake), and set the options' defaults below for it.
set(NEXUS_IOS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    include(cmake/ios.cmake)
endif()

# A static library of ours compiles without waiting for the static libraries it links, which only its users' links
# need: so nexus_sync, and every library over it, compiles beside schema's generated code (nexus_wire) rather than after
# it, and CI's 2-minute build is not one long chain. Set after Prax, so Prax's own targets keep their order.
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)

if(MSVC)
    set(NEXUS_WARNINGS /W4 /WX)
    # getenv, strerror and the like are standard C, which MSVC's CRT deprecates in favour of its own; Prax does the same.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
    set(NEXUS_WARNINGS -Wall -Wextra -Wpedantic -Werror)
endif()

# The window: the app and the libraries and tests that draw or set text (the view, the terminal tabs, the sprint tab).
# On by default on macOS and Windows: Prax's lib/app has backends for Cocoa, Win32 and the web, none for Linux, and its
# lib/font finds the system's fonts through CoreText and DirectWrite only. On Windows the app builds against Prax's
# Win32 backend, and its link is nexus-link-relay inside WSL2 (src/relay/relay_wire.h). Off, everything else still
# builds and tests: the model, the link's states, the store, the driver protocol and supervisor, nexus-server, and the
# window's logic.
if(APPLE OR WIN32)
    set(nexus_window_default ON)
else()
    set(nexus_window_default OFF)
endif()
option(NEXUS_WINDOW "Build the window (the app, its view, terminals and sprint tab, and their tests)"
    ${nexus_window_default})
# The window's logic (src/command.h runCommand, and what uses it: the remote and sprint logic, the link, nexus-server and
# their tests) starts processes through Prax's lib/process. On by default on macOS and Linux (and so inside WSL2), where
# lib/process's POSIX backend runs (on Linux over the workaround above, "Prax's lib/process on Linux"), and on Windows,
# where it runs over its Win32 backend and the window needs it.
option(NEXUS_WINDOW_LOGIC "Build the window's logic over Prax's lib/process, and what uses it (the link included)" ON)
if(NEXUS_WINDOW AND NOT NEXUS_WINDOW_LOGIC)
    message(FATAL_ERROR "nexus: the window needs its logic; configure with -DNEXUS_WINDOW_LOGIC=ON or -DNEXUS_WINDOW=OFF")
endif()
if(NEXUS_WINDOW AND NOT APPLE AND NOT WIN32)
    message(STATUS "nexus: the window's libraries and their tests are built, but not the app, whose platform layer is "
        "macOS's (src/platform_mac.mm): Prax's lib/app has no backend here")
endif()

# The link between the window and nexus-server: mosh's transport (cmake/mosh.cmake), which needs protobuf. OFF builds
# without nexus-server and without the window's --server; the model checks need neither. It uses the window's logic, so
# it is on by default where that is.
# On Windows only the window (the UI overlay) is native: nexus-server and the mosh link run as the Linux build inside
# WSL2: the native window starts the window's link there, nexus-link-relay, and talks to it over its standard input and
# output (src/relay/relay_wire.h says why). So none of them is built natively on Windows: the link
# is off there and cannot be turned on, and nexus-server's POSIX parts (the real filesystem under the store, the
# drivers' poll loop, the fake driver and their functional tests) are left out below.
if(WIN32)
    set(nexus_link_default OFF)
else()
    set(nexus_link_default ${NEXUS_WINDOW_LOGIC})
endif()
option(NEXUS_LINK "Build nexus-server and the window's --server over mosh's transport (needs protobuf)"
    ${nexus_link_default})
# The client lock's stand-in authenticator (src/lock_test_authenticator.cpp), for running the window where no one can
# authenticate. A build with it unlocks with no real check: never on, never shipped (docs/security.md).
option(NEXUS_TEST_AUTHENTICATOR "Build the app with the lock's stand-in authenticator (testing only, never shipped)" OFF)
if(WIN32)
    if(NEXUS_LINK)
        message(FATAL_ERROR "nexus: on Windows nexus-server and the mosh link run as the Linux build inside WSL2, and "
            "only the window is built natively; configure with -DNEXUS_LINK=OFF (build the link and nexus-server inside "
            "WSL2 instead)")
    endif()
    message(STATUS "nexus: Windows builds the window only; nexus-server, the mosh link and the store's POSIX "
        "filesystem run as the Linux build inside WSL2, and are not built here")
endif()
# nexus-server's own parts (the store, the server's tabs on it, the driver protocol and the driver supervisor) and
# their tests are built where nexus-server runs: macOS and Linux, and so inside WSL2 on Windows. A native Windows build
# leaves them out, and keeps every library the window links (the model, the link's states with the accounts, the
# settings files, the hitch detector, the board's scroller, the terminal tab's launch line), the formats' tables the
# window and the server will share (nexus_wire, docs/formats.md), their unit tests and the WSL2 terminal test
# (tests/window wsl2_terminal). The model checks' walks are left out there too: TLC runs them on
# Linux and macOS (tests/model tlc_test).
if(WIN32 OR NEXUS_IOS)
    set(NEXUS_SERVER_PARTS OFF)
else()
    set(NEXUS_SERVER_PARTS ON)
endif()
if(NEXUS_LINK AND NOT NEXUS_WINDOW_LOGIC)
    message(FATAL_ERROR "nexus: the link (nexus-server and the window's --server) uses the window's logic, which is off "
        "(NEXUS_WINDOW_LOGIC). Configure with -DNEXUS_LINK=OFF, or with -DNEXUS_WINDOW_LOGIC=ON.")
endif()
if(NEXUS_LINK)
    include(cmake/mosh.cmake)
endif()

# nexus's built-in Tailscale (docs/tailscale.md): the Go shim over tsnet (src/tailscale), built with the pinned Go
# toolchain, and its tests. Layer 0 builds it and nothing links it yet, so it is off by default and needs Go when on;
# src/tailscale/CMakeLists.txt builds it on its own, without Prax, as CI does.
option(NEXUS_TAILSCALE "Build the Tailscale shim (src/tailscale) and its tests (needs Go)" OFF)
if(NEXUS_TAILSCALE)
    include(cmake/tailscale.cmake)
endif()

# What Claude's sandbox needs on Linux and WSL2, bubblewrap and socat, built from pinned sources so no machine installs
# them (cmake/sandbox.cmake); nexus-server, whose drivers use them, depends on them. macOS's sandbox is the system's.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(nexus_sandbox_default ${NEXUS_LINK})
else()
    set(nexus_sandbox_default OFF)
endif()
option(NEXUS_SANDBOX_TOOLS "Build bubblewrap and socat for the Claude driver's sandbox (Linux only)"
    ${nexus_sandbox_default})
if(NEXUS_SANDBOX_TOOLS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "nexus: bubblewrap and socat are the sandbox's on Linux only; configure with "
            "-DNEXUS_SANDBOX_TOOLS=OFF")
    endif()
    include(cmake/sandbox.cmake)
endif()

include(cmake/policy.cmake)

# The non-drawing logic: tabs, transcripts, drafts, send and queue, and when the overlay shows and hides (overlay.h).
# No OS, no time.
add_library(nexus_model STATIC src/model.cpp src/overlay.cpp src/cursor.cpp src/client_lock.cpp)
target_include_directories(nexus_model PUBLIC src)
target_compile_options(nexus_model PRIVATE ${NEXUS_WARNINGS})

# The accounts and AIs nexus-server holds (src/accounts/registry.h): their rules, their JSON files' text (with
# Prax's JSON), and the accounts the server picks. No OS, no time.
add_library(nexus_accounts STATIC src/accounts/registry.cpp)
target_include_directories(nexus_accounts PUBLIC src)
target_compile_options(nexus_accounts PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_accounts PUBLIC prax::json prax::core nexus_policy)

# The two states mosh's transport keeps in step between the window and nexus-server (the person's actions up, the tabs
# down), what the server does with an action, and what the window shows of the tabs. No OS, no time.
# From state format 4 the bytes are schema's tables (nexus_wire, cmake/schema.cmake); before it the hand-written codecs.
add_library(nexus_sync STATIC src/sync/wire.cpp src/sync/formats.cpp src/sync/actions.cpp src/sync/sessions.cpp
    src/sync/legacy_codec.cpp src/sync/tables_codec.cpp src/sync/tables_actions.cpp
    src/sync/hub.cpp src/sync/mirror.cpp src/sync/client.cpp src/sync/ai_strip.cpp src/sync/secrets.cpp
    src/sync/signin.cpp src/sync/redial.cpp src/sync/dashboards.cpp)
target_include_directories(nexus_sync PUBLIC src)
target_compile_options(nexus_sync PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_sync PUBLIC nexus_model nexus_accounts nexus_wire nexus_policy)

if(NEXUS_SERVER_PARTS)
    # nexus-server's durable state: the tabs and each client's applied counter over a crash-safe journal, behind a
    # filesystem seam (src/store/fs.h) so that its tests run in memory. No Prax, no OS beyond the files. The real
    # filesystem (make_posix_fs) is POSIX, as nexus-server is.
    add_library(nexus_store STATIC src/store/codec.cpp src/store/journal.cpp src/store/mem_fs.cpp src/store/store.cpp
        src/store/posix_fs.cpp)
    target_include_directories(nexus_store PUBLIC src)
    target_compile_options(nexus_store PRIVATE ${NEXUS_WARNINGS})

    # nexus-server's tabs on the store: the Hub's TabStore over src/store (no mosh, so its tests build without the
    # link), the secret dialog's account files over the same filesystem seam (src/server/secret_files.h), and where its
    # answers and log lines go (src/server/secret_lines.h, which writes the driver protocol's `secret` line).
    add_library(nexus_durable STATIC src/server/durable.cpp src/server/secret_files.cpp src/server/secret_lines.cpp
                src/server/login_life.cpp)
    target_include_directories(nexus_durable PUBLIC src)
    target_compile_options(nexus_durable PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_durable PUBLIC nexus_sync nexus_store nexus_driver)

    # The driver protocol (docs/driver-protocol.md): the lines nexus-server and an AI's driver process exchange,
    # their encoding and decoding, and a reader that cuts a stream into them. No OS, no time, no processes.
    add_library(nexus_driver STATIC src/driver/protocol.cpp)
    target_include_directories(nexus_driver PUBLIC src)
    target_compile_options(nexus_driver PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_driver PUBLIC prax::json prax::core)

    # The driver supervisor (src/server/supervisor.h): what nexus-server does with one session's driver, as a pure
    # state machine; the drivers file (src/server/driver_table.h); and the server's home and config
    # (src/server/home.h). No OS, no time, no processes.
    add_library(nexus_supervisor STATIC src/server/supervisor.cpp src/server/driver_table.cpp src/server/home.cpp)
    target_include_directories(nexus_supervisor PUBLIC src)
    target_compile_options(nexus_supervisor PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_supervisor PUBLIC nexus_sync nexus_driver nexus_config)

    # The AIs' driver processes in nexus-server's poll loop (src/server/drivers.h), over the supervisor. POSIX
    # (poll, process ids), like the rest of nexus-server.
    add_library(nexus_drivers STATIC src/server/drivers.cpp)
    target_include_directories(nexus_drivers PUBLIC src)
    target_compile_options(nexus_drivers PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_drivers PUBLIC nexus_supervisor)

    add_library(nexus_services STATIC src/server/service_process.cpp src/server/service_manifest.cpp)
    target_include_directories(nexus_services PUBLIC src)
    target_compile_options(nexus_services PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_services PRIVATE nexus_accounts prax::json)

    # In-app sign-in's processes (src/server/signin_runner.h): `claude setup-token` under a pty, the helper's socket,
    # the callback's replay on loopback; and the helper itself, nexus-signin-open, which stands in for a browser.
    add_library(nexus_signin_runner STATIC src/server/signin_runner.cpp)
    target_include_directories(nexus_signin_runner PUBLIC src)
    target_compile_options(nexus_signin_runner PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_signin_runner PUBLIC nexus_sync PRIVATE nexus_durable)
    configure_file(src/server/nexus-claude-auth ${CMAKE_CURRENT_BINARY_DIR}/nexus-claude-auth COPYONLY)
    add_custom_target(nexus-claude-auth)
    add_executable(nexus-signin-open src/server/signin_open.cpp)
    target_compile_options(nexus-signin-open PRIVATE ${NEXUS_WARNINGS})

    # The accounts and AIs' config repository (src/server/config_repo.h): the files, and git as a child process.
    add_library(nexus_config_repo STATIC src/server/config_repo.cpp)
    target_include_directories(nexus_config_repo PUBLIC src)
    target_compile_options(nexus_config_repo PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_config_repo PUBLIC nexus_sync)

    # Placement (src/server/builds.h, docs/builds.md): the installed builds, each immutable and named by its content's
    # hash, and the one pointer to the current and previous. POSIX files and locks; no time but what is passed in.
    add_library(nexus_builds STATIC src/server/sha256.cpp src/server/build_manifest.cpp src/server/builds.cpp
                src/server/migration.cpp)
    target_include_directories(nexus_builds PUBLIC src)
    target_compile_options(nexus_builds PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_builds PUBLIC nexus_store)
    target_link_libraries(nexus_supervisor PRIVATE nexus_builds)

    # Handover (src/server/handover.h, src/server/drain.h, docs/builds.md): durable, resumable handover state machine
    # under an OS lock, draining the old server and rolling back to previous when a target build fails to serve.
    add_library(nexus_handover STATIC src/server/drain.cpp src/server/supervise_state.cpp src/server/handover.cpp)
    target_include_directories(nexus_handover PUBLIC src)
    target_compile_options(nexus_handover PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_handover PUBLIC nexus_builds nexus_store)

    # Supervision (src/server/supervision.h, docs/builds.md): keeps the current placed build serving across boots,
    # drain handovers (exit 75), intentional stops (exit 0), and post-start crashes, and generates OS unit files.
    add_library(nexus_supervision STATIC src/server/supervision.cpp)
    target_include_directories(nexus_supervision PUBLIC src)
    target_compile_options(nexus_supervision PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_supervision PUBLIC nexus_handover nexus_builds nexus_store)
endif()

# The window's gate for in-app sign-in with the server on another machine (src/signin_gate.h): one loopback listener
# for one callback, on a thread of its own. Built everywhere, Windows included, where the window runs.
find_package(Threads REQUIRED)
add_library(nexus_signin_gate STATIC src/signin_gate.cpp)
target_include_directories(nexus_signin_gate PUBLIC src)
target_compile_options(nexus_signin_gate PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_signin_gate PUBLIC nexus_sync Threads::Threads)
if(WIN32)
    target_link_libraries(nexus_signin_gate PUBLIC ws2_32)
endif()

# The window's hitch detector (src/hitch.h): each frame's main-thread work against the budget, its phases, a fixed ring
# of hitches and the log. No OS, no time (the clock is passed in), no Prax.
add_library(nexus_hitch STATIC src/hitch.cpp)
target_include_directories(nexus_hitch PUBLIC src)
target_compile_options(nexus_hitch PRIVATE ${NEXUS_WARNINGS})

# The sprint board's scrolling (src/scroll_glide.h): the curves and the scroller over its page. No OS, no time, no
# Prax, so that the model job builds its trace walk alone.
include(cmake/ui_defaults.cmake)
add_library(nexus_scroll STATIC src/scroll_glide.cpp)
target_include_directories(nexus_scroll PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(nexus_scroll PRIVATE ${NEXUS_WARNINGS})

# nexus's settings files (src/config.h), the window's ~/.nexus/client/config and nexus-server's
# ~/.nexus/server/config: the parsing only. No OS, no time.
add_library(nexus_config STATIC src/config.cpp)
target_include_directories(nexus_config PUBLIC src)
target_compile_options(nexus_config PRIVATE ${NEXUS_WARNINGS})

# What a terminal tab's shell is (src/terminal_launch.h): the login shell on macOS and Linux, a WSL2 bash on Windows, and
# what the probe of WSL2 found. No OS, no time, no Prax, so it builds and is unit-tested everywhere, Windows included.
add_library(nexus_terminal_launch STATIC src/terminal_launch.cpp)
target_include_directories(nexus_terminal_launch PUBLIC src)
target_compile_options(nexus_terminal_launch PRIVATE ${NEXUS_WARNINGS})

# Synchronization of a terminal tab's foreground process group between the background poll thread and the frame thread
# (src/terminal_watch.h, models/TerminalWatch.tla). No OS, no time, no Prax, so it builds and is unit-tested everywhere.
add_library(nexus_terminal_watch STATIC src/terminal_watch.cpp)
target_include_directories(nexus_terminal_watch PUBLIC src)
target_compile_options(nexus_terminal_watch PRIVATE ${NEXUS_WARNINGS})

# The relay (src/relay/relay_wire.h): how the native window on Windows reaches nexus-server through the link run inside
# WSL2 (nexus-link-relay): its frames, both ends as bytes in and out, and the window's side of the relays' life
# (relay_life.h, models/RelayLife.tla). No OS, no time, so it builds and is unit-tested
# everywhere; the relay itself is built with the link (below), and the window's port over wsl.exe on Windows.
add_library(nexus_relay_wire STATIC src/relay/relay_wire.cpp src/relay/relay_ends.cpp src/relay/relay_life.cpp
    src/relay/relay_words.cpp)
target_include_directories(nexus_relay_wire PUBLIC src)
target_compile_options(nexus_relay_wire PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_relay_wire PUBLIC nexus_sync)

# The formats across versions as schema's variable tables (schema/Nexus.schema), generated and checked in under
# schema/gen: the library nexus_wire, and the target schema_generate that regenerates it at the pinned schema commit.
include(cmake/schema.cmake)

if(NEXUS_WINDOW_LOGIC)
    # The window's logic that runs without a window: the AI and machine lists' parsing (and the command that prints
    # them), a terminal tab's label in ssh (remote.h), the saved tab order, the window's home and config (client_home.h),
    # and the sprint tab's (where the board is served, which events go to it, the script nexus adds to it).
    add_library(nexus_window_logic STATIC src/command.cpp src/remote.cpp src/sprint.cpp src/tab_order.cpp
        src/client_home.cpp src/server_choice.cpp src/frame_trace.cpp)
    target_include_directories(nexus_window_logic PUBLIC src)
    target_compile_options(nexus_window_logic PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_window_logic PUBLIC nexus_config nexus_model nexus_accounts nexus_sync nexus_scroll prax::app prax::json prax::process prax::core_cpp)
endif()

if(NEXUS_WINDOW)
    # The window's tree over Prax's UI context and Studio Kit: built by the app, and headless by tests/window/.
    add_library(nexus_view STATIC src/view.cpp src/chat_markdown.cpp src/chat_selection.cpp src/window_app.cpp src/native_tabs.cpp src/font_prefetch.cpp)
    target_include_directories(nexus_view PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_options(nexus_view PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_view PUBLIC nexus_policy nexus_window_logic nexus_hitch
        prax::markdown prax::studio_kit prax::ui prax::ui_cpp prax::layout prax::font prax::text prax::draw prax::app prax::core_cpp)

    # The terminal tabs' shells over Prax's terminal (view.h Terminals): built by the app, and run by the functional test.
    add_library(nexus_terminals STATIC src/terminals.cpp src/foreground.cpp)
    target_include_directories(nexus_terminals PUBLIC src)
    target_compile_options(nexus_terminals PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_terminals PUBLIC nexus_view nexus_terminal_launch nexus_terminal_watch
        prax::studio_terminal prax::process prax::vterm prax::core prax::core_cpp)

    # The sprint tab's page over Prax's HTML view, scripts and network (view.h Board): built by the app, and run by the
    # functional test on pages it serves from memory.
    add_library(nexus_board STATIC src/board.cpp)
    target_compile_definitions(nexus_board PRIVATE NEXUS_DISPLAY_VERSION="v${PROJECT_VERSION}")
    target_include_directories(nexus_board PUBLIC src)
    target_compile_options(nexus_board PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_board PUBLIC nexus_view prax::html prax::html_quickjs prax::svg_quickjs prax::url_quickjs
        prax::dom prax::fetch prax::net prax::tls prax::http prax::image_select prax::compress_select prax::text prax::font prax::draw
        prax::layout prax::ui prax::studio_kit prax::app prax::core_cpp)

    # What the window does around the view each frame (src/frame_host.h), and its links opened in the system's browser
    # off the frame thread (src/url_opener.h): built by the app, and driven by the frame-budget test.
    add_library(nexus_frame STATIC src/frame_host.cpp src/url_opener.cpp src/window_lock.cpp)
    target_include_directories(nexus_frame PUBLIC src)
    target_compile_options(nexus_frame PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_frame PUBLIC nexus_view nexus_terminals nexus_board nexus_sync nexus_hitch
        nexus_signin_gate Threads::Threads)
    if(WIN32)
        target_link_libraries(nexus_frame PUBLIC shell32 ole32)
    endif()
endif()

if(NEXUS_LINK)
    # The link over mosh's transport: its sessions (both ends), the ssh bootstrap, and the window's link thread.
    add_library(nexus_link STATIC src/link/mosh_link.cpp src/link/bootstrap.cpp src/link/client.cpp)
    target_include_directories(nexus_link PUBLIC src)
    target_compile_options(nexus_link PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_link PUBLIC nexus_sync nexus_window_logic mosh_transport)

    # The server the window talks to with --server: headless, on the machine that keeps going.
    add_library(nexus_bus STATIC src/server/bus_listener.cpp src/server/bus_attachment.cpp
        src/server/bus_grants.cpp src/server/bus_inbox.cpp)
    target_compile_options(nexus_bus PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_bus PUBLIC nexus_link nexus_builds nexus_drivers)
    add_library(nexus_server STATIC src/server/server.cpp src/server/flush_gate.cpp)
    target_compile_options(nexus_server PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_server PUBLIC nexus_bus nexus_policy nexus_link nexus_durable nexus_drivers nexus_config_repo nexus_builds
        nexus_signin_runner nexus_handover nexus_supervision nexus_services)
    add_executable(nexus-server src/server/main.cpp)
    target_compile_options(nexus-server PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus-server PRIVATE nexus_server)
    if(NEXUS_SANDBOX_TOOLS)
        add_dependencies(nexus-server nexus_sandbox_tools)
    endif()
    # The window's link for the native window on Windows, run inside WSL2 (src/link/relay_main.cpp, relay/relay_wire.h).
    add_executable(nexus-link-relay src/link/relay_main.cpp)
    target_compile_options(nexus-link-relay PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus-link-relay PRIVATE nexus_link nexus_relay_wire)
    # The helper goes beside nexus-server: the server finds it there.
    add_dependencies(nexus-server nexus-signin-open)
    # A test's alone, never shipped: nexus-server built to write each forwarded sign-in callback's query to its log, the
    # reversed witness that link_e2e_signin's search of server.log finds such a leak (link_e2e_signin_leaky). Built
    # only as link_e2e's dependency.
    add_library(nexus_server_leaky STATIC EXCLUDE_FROM_ALL src/server/server.cpp src/server/flush_gate.cpp)
    target_compile_options(nexus_server_leaky PRIVATE ${NEXUS_WARNINGS})
    target_compile_definitions(nexus_server_leaky PRIVATE NEXUS_LEAKY_SIGNIN_WITNESS=1)
    target_link_libraries(nexus_server_leaky PUBLIC nexus_bus nexus_policy nexus_link nexus_durable nexus_drivers nexus_config_repo
        nexus_builds nexus_signin_runner nexus_handover nexus_supervision nexus_services)
    add_executable(nexus-server-leaky EXCLUDE_FROM_ALL src/server/main.cpp)
    target_compile_options(nexus-server-leaky PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus-server-leaky PRIVATE nexus_server_leaky)
    add_dependencies(nexus-server-leaky nexus-signin-open)
endif()

if(NEXUS_WINDOW AND (APPLE OR WIN32))
    # The app: on macOS a real bundle, nexus.app; on Windows a GUI program (no console window opens at launch, so
    # launch shows nothing), which still starts at main(). The system's part of the overlay is src/platform.h.
    if(APPLE)
        add_executable(nexus MACOSX_BUNDLE src/main.cpp src/platform_mac.mm src/lock_mac.mm)
        set(NEXUS_MAC_SIGN_IDENTITY "" CACHE STRING "Signing identity for local nexus.app builds")
        if(NEXUS_MAC_SIGN_IDENTITY)
            add_custom_command(TARGET nexus POST_BUILD
                COMMAND /usr/bin/codesign --force --sign "${NEXUS_MAC_SIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:nexus>"
                VERBATIM)
        endif()
        set_source_files_properties(src/platform_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set_source_files_properties(src/lock_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(nexus PRIVATE "-framework AppKit" "-framework CoreGraphics" "-framework LocalAuthentication")
        # The app's name is nexus: the bundle's name and display name (cmake/Info.plist.in, CMake's
        # template with CFBundleDisplayName added), the executable (the process's name in Activity Monitor and the app
        # menu's About, Hide and Quit) and the window's title (main.cpp).
        set_target_properties(nexus PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/cmake/Info.plist.in
            MACOSX_BUNDLE_GUI_IDENTIFIER com.robotgamedevelopers.nexus
            MACOSX_BUNDLE_BUNDLE_NAME nexus
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
            MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    else()
        add_executable(nexus WIN32 src/main.cpp src/platform_win.cpp src/lock_win.cpp)
        # The window's link on Windows: nexus-link-relay inside WSL2, over wsl.exe (src/relay/relay_port.h).
        target_compile_definitions(nexus PRIVATE NEXUS_RELAY=1)
        target_link_libraries(nexus PRIVATE nexus_relay_port)
        if(MSVC)
            target_link_options(nexus PRIVATE /ENTRY:mainCRTStartup)
        endif()
        # Windows Hello (UserConsentVerifier, C++/WinRT) and the session notices (lock_win.cpp).
        target_link_libraries(nexus PRIVATE windowsapp wtsapi32)
    endif()
    # The client lock (client_lock.h): the system's authenticator, or with NEXUS_TEST_AUTHENTICATOR (a test build that
    # checks no one, for running and screenshotting the window where no one can touch the sensor; never a release) the
    # stand-in in lock_test_authenticator.cpp.
    # A Release build (any optimised build type, or a multi-config generator, which can build Release) or a signed one
    # is what ships: the stand-in in it is a configure error, not a warning.
    if(NEXUS_TEST_AUTHENTICATOR)
        if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$"
                OR (APPLE AND NEXUS_MAC_SIGN_IDENTITY))
            message(FATAL_ERROR "NEXUS_TEST_AUTHENTICATOR is ON in a Release, multi-config or signed build "
                "(CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}): the stand-in unlocks with no real check and is never shipped. "
                "Build it as Debug, unsigned, or turn it off.")
        endif()
        message(WARNING "NEXUS_TEST_AUTHENTICATOR is ON: this nexus unlocks with no real check. Never ship it.")
        target_sources(nexus PRIVATE src/lock_test_authenticator.cpp)
        target_compile_definitions(nexus PRIVATE NEXUS_TEST_AUTHENTICATOR=1)
    else()
        # The shipped binary has none of the stand-in: checked after every link, and by the unit test below, which CI runs
        # on its Release build.
        set(nexus_no_stand_in "${CMAKE_CURRENT_SOURCE_DIR}/cmake/no_test_authenticator.cmake")
        add_custom_command(TARGET nexus POST_BUILD
            COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:nexus> -P ${nexus_no_stand_in}
            VERBATIM)
    endif()
    target_sources(nexus PRIVATE src/ui_audio.cpp src/offscreen.cpp)
    target_link_libraries(nexus PRIVATE prax::audio prax::png)
    target_compile_options(nexus PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus PRIVATE nexus_policy nexus_frame nexus_view nexus_terminals nexus_board
        prax::studio_app prax::app_select prax::gpu_select prax::draw_select prax::image_select prax::compress_select)
    if(NEXUS_LINK)
        target_compile_definitions(nexus PRIVATE NEXUS_LINK=1)
        target_link_libraries(nexus PRIVATE nexus_link nexus_sync)
    endif()
endif()

if(WIN32 AND NOT NEXUS_WINDOW)
    # A Windows configure with the window off (-DNEXUS_WINDOW=OFF) still compiles its Windows part,
    # src/platform_win.cpp and src/lock_win.cpp (never linked), so it is held to the same warnings.
    add_library(nexus_platform_win_check OBJECT src/platform_win.cpp src/lock_win.cpp)
    target_compile_options(nexus_platform_win_check PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_platform_win_check PRIVATE prax::surface nexus_model)
endif()

if(WIN32)
    # The window's port to nexus-link-relay in WSL2 (src/relay/relay_port.h): the process, its pipes and threads.
    add_library(nexus_relay_port STATIC src/relay/relay_port_win.cpp)
    target_include_directories(nexus_relay_port PUBLIC src)
    target_compile_options(nexus_relay_port PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_relay_port PUBLIC nexus_relay_wire Threads::Threads)
endif()

enable_testing()
# The TLC model checks run in the nightly alone, and a nightly test has no time limit (only per-commit unit and
# functional tests have one, 2 minutes): there, NEXUS_TLC_UNCAPPED=ON drops each check's cap and ctest's timeout, and a
# check runs until TLC finishes. A configure without it (a local run) keeps each check's cap.
option(NEXUS_TLC_UNCAPPED "Run the TLC model checks with no time cap (the nightly)" OFF)
# nexus_tlc_limits(<cap> <cap-var> <timeout-var>): <cap-var> is the cap tlc.cmake applies (0 for none), and
# <timeout-var> the TIMEOUT property for set_tests_properties (empty for none).
function(nexus_tlc_limits cap cap_var timeout_var)
    if(NEXUS_TLC_UNCAPPED)
        set(${cap_var} 0 PARENT_SCOPE)
        set(${timeout_var} "" PARENT_SCOPE)
    else()
        math(EXPR timeout "${cap} + 30")
        set(${cap_var} ${cap} PARENT_SCOPE)
        set(${timeout_var} TIMEOUT ${timeout} PARENT_SCOPE)
    endif()
endfunction()
# The shipped app has none of the lock's stand-in authenticator (cmake/no_test_authenticator.cmake; CMake option
# NEXUS_TEST_AUTHENTICATOR).
if(TARGET nexus AND NOT NEXUS_TEST_AUTHENTICATOR)
    add_test(NAME nexus_no_test_authenticator
        COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:nexus> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/no_test_authenticator.cmake)
    set_tests_properties(nexus_no_test_authenticator PROPERTIES LABELS unit TIMEOUT 30)
    add_test(NAME nexus_no_test_authenticator_detects
        COMMAND ${CMAKE_COMMAND} -DBINARY=${CMAKE_CURRENT_SOURCE_DIR}/cmake/no_test_authenticator.cmake
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/no_test_authenticator.cmake)
    set_tests_properties(nexus_no_test_authenticator_detects PROPERTIES LABELS unit TIMEOUT 30
        PASS_REGULAR_EXPRESSION "no_test_authenticator:FOUND")
endif()
# NEXUS_LTO reaches Prax's targets (tests/build/lto_check.cmake): a configure-time record, checked in a moment.
add_test(NAME build_unit_lto COMMAND ${CMAKE_COMMAND} "-DRECORD=${CMAKE_BINARY_DIR}/prax-ipo.txt"
    -P "${PROJECT_SOURCE_DIR}/tests/build/lto_check.cmake")
set_tests_properties(build_unit_lto PROPERTIES LABELS unit TIMEOUT 30)

# The policy tool runs on the build machine, so the reload check runs in the desktop trees; an iOS tree builds for a
# simulator or a device, where a test is a program the simulator runner starts (tests/ios). NEXUS_POLICY_TOOL_TESTS
# says which (tests/tiers.txt names these tests where it is on).
set(NEXUS_POLICY_TOOL_TESTS OFF)
if(TARGET nexus_policy AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(NEXUS_POLICY_TOOL_TESTS ON)
    add_executable(policy_reload_test tests/policy/reload.cpp)
    target_link_libraries(policy_reload_test PRIVATE nexus_policy)
    target_compile_options(policy_reload_test PRIVATE ${NEXUS_WARNINGS})
    add_test(NAME policy_functional_reload COMMAND ${CMAKE_COMMAND}
        "-DTOOL=${policy_tool}" "-DCHECK=$<TARGET_FILE:policy_reload_test>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/policy-reload-test" -P "${PROJECT_SOURCE_DIR}/tests/policy/reload.cmake")
    set_tests_properties(policy_functional_reload PROPERTIES LABELS functional TIMEOUT 30)
    # An applied bundle that is not the one the binary packages: the comparison, and what apply writes from this commit.
    add_executable(policy_stale_bundle_test tests/policy/stale_bundle.cpp)
    target_link_libraries(policy_stale_bundle_test PRIVATE nexus_policy)
    target_compile_options(policy_stale_bundle_test PRIVATE ${NEXUS_WARNINGS})
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/policy-stale-bundle-test")
    add_test(NAME policy_unit_stale_bundle COMMAND policy_stale_bundle_test
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/policy-stale-bundle-test")
    add_test(NAME policy_unit_stale_bundle_applied COMMAND ${CMAKE_COMMAND}
        "-DTOOL=${policy_tool}" "-DCHECK=$<TARGET_FILE:policy_stale_bundle_test>" "-DRULES=${PROJECT_SOURCE_DIR}/policy/rules"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/policy-stale-bundle-applied" -P "${PROJECT_SOURCE_DIR}/tests/policy/stale_bundle_apply.cmake")
    set_tests_properties(policy_unit_stale_bundle policy_unit_stale_bundle_applied PROPERTIES LABELS unit TIMEOUT 30)
    add_test(NAME policy_functional_old_names COMMAND ${CMAKE_COMMAND} "-DTOOL=${policy_tool}"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/policy-old-names" "-DRULES=${PROJECT_SOURCE_DIR}/policy/rules"
        -P "${PROJECT_SOURCE_DIR}/tests/policy/old_names.cmake")
    set_tests_properties(policy_functional_old_names PROPERTIES LABELS functional TIMEOUT 30)
endif()
add_subdirectory(tests/model)
add_subdirectory(tests/sync)
add_subdirectory(tests/accounts)
add_subdirectory(tests/window)
if(NEXUS_SERVER_PARTS)
    add_subdirectory(tests/store)
    add_subdirectory(tests/driver)
    add_subdirectory(tests/server)
endif()
add_subdirectory(tests/wire)
add_subdirectory(tests/relay)
if(NEXUS_LINK)
    add_subdirectory(tests/link)
    # The mock (docs/mock.md): a real nexus-server with fake AIs, for driving the clients. POSIX, as the fake driver is.
    if(NOT WIN32)
        add_subdirectory(tests/mock)
    endif()
endif()
if(NEXUS_TAILSCALE)
    add_subdirectory(tests/tailscale)
endif()
# The watch's core, its tests and models, and on macOS with the link the phone's relay host (watch/CMakeLists.txt).
# Not in the iOS tree: its models run on the build machine (watch/tests/tlc.cmake), which an iOS build cannot run.
if(NOT WIN32 AND NOT NEXUS_IOS)
    add_subdirectory(watch)
endif()
# Each test's tier (tier_pr or tier_nightly, cmake/tiers.cmake and tests/tiers.txt), given once every test above is
# registered.
include(cmake/tiers.cmake)
