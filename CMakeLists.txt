# nexus: one person and several AIs in one window. The window runs on macOS; on 64-bit Windows it builds against Prax's
# Win32 backend where Prax allows (NEXUS_WINDOW below), and a native Windows build holds the window's code alone
# (NEXUS_SERVER_PARTS below); on Linux the parts without a window build and test.
cmake_minimum_required(VERSION 3.28)

# Prax runs on macOS 26 or newer; set before project(), which reads it.
set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "Oldest macOS the build runs on")

project(nexus LANGUAGES C CXX)
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

# Prax options: leave out what this window does not use. The sprint tab's page runs its scripts in QuickJS (PR_JS).
set(PR_JS ON CACHE BOOL "" FORCE)
set(PR_CHARSET_LEGACY OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(prax
    URL https://codeload.github.com/pdoane/prax/tar.gz/e39b205bbba4845077889f4a0fbfb38525d943ff
    URL_HASH SHA256=a323930502458e28586f85f6bac1e5af8bf17502753f9b26fc2dde141fab9ad5
    # SYSTEM: Prax's headers use Clang nullability, which -Wpedantic rejects in our code otherwise.
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(prax)

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
# On by default on macOS only: Prax's lib/app has backends for Cocoa, Win32 and the web, none for Linux, and its
# lib/font finds the system's fonts through CoreText and DirectWrite only. On Windows, ON builds the app against Prax's
# Win32 backend, which does not compile at this pin (lib/app/backends/win32/src/app_win32.cpp), so it is off there too. Off, everything else still builds and tests:
# the model, the link's states, the store, the driver protocol and supervisor, nexus-server, and the window's logic.
option(NEXUS_WINDOW "Build the window (the app, its view, terminals and sprint tab, and their tests)" ${APPLE})
# The window's logic (src/command.h runCommand, and what uses it: the remote and sprint logic, the link, nexus-server and
# their tests) starts processes through Prax's lib/process. On by default on macOS only: at this Prax pin, lib/process's
# POSIX backend does not link on Linux (process_posix.cpp declares `environ` inside its own namespace, so the symbol is
# never defined), so off macOS those parts are left out until Prax fixes it.
option(NEXUS_WINDOW_LOGIC "Build the window's logic over Prax's lib/process, and what uses it (the link included)" ${APPLE})
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
# WSL2, which the native window will reach over a local socket. So none of them is built natively on Windows: the link
# is off there and cannot be turned on, and nexus-server's POSIX parts (the real filesystem under the store, the
# drivers' poll loop, the fake driver and their functional tests) are left out below.
if(WIN32)
    set(nexus_link_default OFF)
else()
    set(nexus_link_default ${NEXUS_WINDOW_LOGIC})
endif()
option(NEXUS_LINK "Build nexus-server and the window's --server over mosh's transport (needs protobuf)"
    ${nexus_link_default})
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
if(WIN32)
    set(NEXUS_SERVER_PARTS OFF)
else()
    set(NEXUS_SERVER_PARTS ON)
endif()
if(NEXUS_LINK AND NOT NEXUS_WINDOW_LOGIC)
    message(FATAL_ERROR "nexus: the link (nexus-server and the window's --server) uses the window's logic, which is off "
        "(NEXUS_WINDOW_LOGIC: Prax's lib/process does not link on this platform at this pin). Configure with "
        "-DNEXUS_LINK=OFF, or -DNEXUS_WINDOW_LOGIC=ON where Prax's lib/process links.")
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
add_library(nexus_model STATIC src/model.cpp src/overlay.cpp src/cursor.cpp)
target_include_directories(nexus_model PUBLIC src)
target_compile_options(nexus_model PRIVATE ${NEXUS_WARNINGS})

# The accounts and friends nexus-server holds (src/accounts/registry.h): their rules, their JSON files' text (with
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
    src/sync/hub.cpp src/sync/mirror.cpp src/sync/client.cpp src/sync/friend_strip.cpp src/sync/secrets.cpp
    src/sync/signin.cpp src/sync/redial.cpp)
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

    # The driver protocol (docs/driver-protocol.md): the lines nexus-server and a friend's driver process exchange,
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

    # The friends' driver processes in nexus-server's poll loop (src/server/drivers.h), over the supervisor. POSIX
    # (poll, process ids), like the rest of nexus-server.
    add_library(nexus_drivers STATIC src/server/drivers.cpp)
    target_include_directories(nexus_drivers PUBLIC src)
    target_compile_options(nexus_drivers PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_drivers PUBLIC nexus_supervisor)

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

    # The accounts and friends' config repository (src/server/config_repo.h): the files, and git as a child process.
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
add_library(nexus_scroll STATIC src/scroll_glide.cpp)
target_include_directories(nexus_scroll PUBLIC src)
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

# The formats across versions as schema's variable tables (schema/Nexus.schema), generated and checked in under
# schema/gen: the library nexus_wire, and the target schema_generate that regenerates it at the pinned schema commit.
include(cmake/schema.cmake)

if(NEXUS_WINDOW_LOGIC)
    # The window's logic that runs without a window: the friend and machine lists' parsing (and the command that prints
    # them), a terminal tab's label in ssh (remote.h), the saved tab order, the window's home and config (client_home.h),
    # and the sprint tab's (where the board is served, which events go to it, the script nexus adds to it).
    add_library(nexus_window_logic STATIC src/command.cpp src/remote.cpp src/sprint.cpp src/tab_order.cpp
        src/client_home.cpp)
    target_include_directories(nexus_window_logic PUBLIC src)
    target_compile_options(nexus_window_logic PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_window_logic PUBLIC nexus_config nexus_model nexus_accounts nexus_scroll prax::app prax::json prax::process prax::core_cpp)
endif()

if(NEXUS_WINDOW)
    # The window's tree over Prax's UI context and Studio Kit: built by the app, and headless by tests/window/.
    add_library(nexus_view STATIC src/view.cpp src/window_app.cpp src/native_tabs.cpp src/font_prefetch.cpp)
    target_include_directories(nexus_view PUBLIC src)
    target_compile_options(nexus_view PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_view PUBLIC nexus_policy nexus_window_logic nexus_hitch
        prax::studio_kit prax::ui prax::ui_cpp prax::layout prax::font prax::text prax::draw prax::app prax::core_cpp)

    # The terminal tabs' shells over Prax's terminal (view.h Terminals): built by the app, and run by the functional test.
    add_library(nexus_terminals STATIC src/terminals.cpp src/foreground.cpp)
    target_include_directories(nexus_terminals PUBLIC src)
    target_compile_options(nexus_terminals PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_terminals PUBLIC nexus_view nexus_terminal_launch nexus_terminal_watch
        prax::studio_terminal prax::process prax::vterm prax::core prax::core_cpp)

    # The sprint tab's page over Prax's HTML view, scripts and network (view.h Board): built by the app, and run by the
    # functional test on pages it serves from memory.
    add_library(nexus_board STATIC src/board.cpp)
    target_include_directories(nexus_board PUBLIC src)
    target_compile_options(nexus_board PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_board PUBLIC nexus_view prax::html prax::html_quickjs prax::svg_quickjs prax::url_quickjs
        prax::dom prax::fetch prax::net prax::tls prax::http prax::image prax::compress prax::text prax::font prax::draw
        prax::layout prax::ui prax::studio_kit prax::app prax::core_cpp)

    # What the window does around the view each frame (src/frame_host.h), and its links opened in the system's browser
    # off the frame thread (src/url_opener.h): built by the app, and driven by the frame-budget test.
    add_library(nexus_frame STATIC src/frame_host.cpp src/url_opener.cpp)
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
    add_library(nexus_server STATIC src/server/server.cpp src/server/flush_gate.cpp)
    target_compile_options(nexus_server PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_server PUBLIC nexus_policy nexus_link nexus_durable nexus_drivers nexus_config_repo nexus_builds
        nexus_signin_runner nexus_handover nexus_supervision)
    add_executable(nexus-server src/server/main.cpp)
    target_compile_options(nexus-server PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus-server PRIVATE nexus_server)
    if(NEXUS_SANDBOX_TOOLS)
        add_dependencies(nexus-server nexus_sandbox_tools)
    endif()
    # The helper goes beside nexus-server: the server finds it there.
    add_dependencies(nexus-server nexus-signin-open)
    # A test's alone, never shipped: nexus-server built to write each forwarded sign-in callback's query to its log, the
    # reversed witness that link_e2e_signin's search of server.log finds such a leak (link_e2e_signin_leaky). Built
    # only as link_e2e's dependency.
    add_library(nexus_server_leaky STATIC EXCLUDE_FROM_ALL src/server/server.cpp src/server/flush_gate.cpp)
    target_compile_options(nexus_server_leaky PRIVATE ${NEXUS_WARNINGS})
    target_compile_definitions(nexus_server_leaky PRIVATE NEXUS_LEAKY_SIGNIN_WITNESS=1)
    target_link_libraries(nexus_server_leaky PUBLIC nexus_policy nexus_link nexus_durable nexus_drivers nexus_config_repo
        nexus_builds nexus_signin_runner nexus_handover nexus_supervision)
    add_executable(nexus-server-leaky EXCLUDE_FROM_ALL src/server/main.cpp)
    target_compile_options(nexus-server-leaky PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus-server-leaky PRIVATE nexus_server_leaky)
    add_dependencies(nexus-server-leaky nexus-signin-open)
endif()

if(NEXUS_WINDOW AND (APPLE OR WIN32))
    # The app: on macOS a real bundle, nexus.app; on Windows a GUI program (no console window opens at launch, so
    # launch shows nothing), which still starts at main(). The system's part of the overlay is src/platform.h.
    if(APPLE)
        add_executable(nexus MACOSX_BUNDLE src/main.cpp src/platform_mac.mm)
        set_source_files_properties(src/platform_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(nexus PRIVATE "-framework AppKit")
        # The app's name is nexus: the bundle's name and display name (cmake/Info.plist.in, CMake's
        # template with CFBundleDisplayName added), the executable (the process's name in Activity Monitor and the app
        # menu's About, Hide and Quit) and the window's title (main.cpp).
        set_target_properties(nexus PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/cmake/Info.plist.in
            MACOSX_BUNDLE_GUI_IDENTIFIER com.robotgamedevelopers.nexus
            MACOSX_BUNDLE_BUNDLE_NAME nexus
            MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1.0
            MACOSX_BUNDLE_BUNDLE_VERSION 0.1.0)
    else()
        add_executable(nexus WIN32 src/main.cpp src/platform_win.cpp)
        if(MSVC)
            target_link_options(nexus PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()
    target_compile_options(nexus PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus PRIVATE nexus_policy nexus_frame nexus_view nexus_terminals nexus_board
        prax::studio_app prax::gpu prax::image prax::compress)
    if(NEXUS_LINK)
        target_compile_definitions(nexus PRIVATE NEXUS_LINK=1)
        target_link_libraries(nexus PRIVATE nexus_link nexus_sync)
    endif()
endif()

if(WIN32 AND NOT NEXUS_WINDOW)
    # The app does not build on Windows until Prax's Win32 backend does (NEXUS_WINDOW above); its Windows part,
    # src/platform_win.cpp, is still compiled (never linked) so CI holds it to the same warnings.
    add_library(nexus_platform_win_check OBJECT src/platform_win.cpp)
    target_compile_options(nexus_platform_win_check PRIVATE ${NEXUS_WARNINGS})
    # Prax's headers by path, not by linking prax::app: any link to it (even $<COMPILE_ONLY:...>) makes the Visual
    # Studio generator build pr_app, which does not compile at the pin.
    target_include_directories(nexus_platform_win_check PRIVATE ${prax_SOURCE_DIR}/lib/core/include
        ${prax_SOURCE_DIR}/lib/app/include)
endif()

enable_testing()
if(TARGET nexus_policy)
    add_executable(policy_reload_test tests/policy/reload.cpp)
    target_link_libraries(policy_reload_test PRIVATE nexus_policy)
    target_compile_options(policy_reload_test PRIVATE ${NEXUS_WARNINGS})
    add_test(NAME policy_functional_reload COMMAND ${CMAKE_COMMAND}
        "-DTOOL=${policy_tool}" "-DCHECK=$<TARGET_FILE:policy_reload_test>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/policy-reload-test" -P "${PROJECT_SOURCE_DIR}/tests/policy/reload.cmake")
    set_tests_properties(policy_functional_reload PROPERTIES LABELS functional TIMEOUT 30)
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
if(NEXUS_LINK)
    add_subdirectory(tests/link)
endif()
if(NEXUS_TAILSCALE)
    add_subdirectory(tests/tailscale)
endif()
