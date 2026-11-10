# nexus: one person and several AIs in one window. macOS only for now.
cmake_minimum_required(VERSION 3.28)

# Prax runs on macOS 26 or newer; set before project(), which reads it.
set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "Oldest macOS the build runs on")

project(nexus LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

# Prax options: leave out what this window does not use. The sprint tab's page runs its scripts in QuickJS (PR_JS).
set(PR_JS ON CACHE BOOL "" FORCE)
set(PR_CHARSET_LEGACY OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(prax
    GIT_REPOSITORY https://github.com/pdoane/prax.git
    GIT_TAG 290852597a6c772d52afc7204f412cf2f04df9bc
    # SYSTEM: Prax's headers use Clang nullability, which -Wpedantic rejects in our code otherwise.
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(prax)

set(NEXUS_WARNINGS -Wall -Wextra -Wpedantic -Werror)

# The link between the window and nexus-server: mosh's transport (cmake/mosh.cmake), which needs protobuf. OFF builds
# without nexus-server and without the window's --server; the model checks need neither.
option(NEXUS_LINK "Build nexus-server and the window's --server over mosh's transport (needs protobuf)" ON)
if(NEXUS_LINK)
    include(cmake/mosh.cmake)
endif()

# The non-drawing logic: tabs, transcripts, drafts, send and queue. No OS, no time.
add_library(nexus_model STATIC src/model.cpp)
target_include_directories(nexus_model PUBLIC src)
target_compile_options(nexus_model PRIVATE ${NEXUS_WARNINGS})

# The two states mosh's transport keeps in step between the window and nexus-server (the person's actions up, the tabs
# down), what the server does with an action, and what the window shows of the tabs. No OS, no time.
add_library(nexus_sync STATIC src/sync/wire.cpp src/sync/actions.cpp src/sync/tabs.cpp src/sync/hub.cpp
    src/sync/mirror.cpp src/sync/client.cpp)
target_include_directories(nexus_sync PUBLIC src)
target_compile_options(nexus_sync PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_sync PUBLIC nexus_model)

# nexus-server's durable state: the tabs and each client's applied counter over a crash-safe journal, behind a
# filesystem seam (src/store/fs.h) so that its tests run in memory. No Prax, no OS beyond the files.
add_library(nexus_store STATIC src/store/codec.cpp src/store/journal.cpp src/store/mem_fs.cpp src/store/posix_fs.cpp
    src/store/store.cpp)
target_include_directories(nexus_store PUBLIC src)
target_compile_options(nexus_store PRIVATE ${NEXUS_WARNINGS})

# nexus-server's tabs on the store: the Hub's TabStore over src/store (no mosh, so its tests build without the link).
add_library(nexus_durable STATIC src/server/durable.cpp)
target_include_directories(nexus_durable PUBLIC src)
target_compile_options(nexus_durable PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_durable PUBLIC nexus_sync nexus_store)

# The window's logic that runs without a window: the friend list's parsing (and the command that prints it), the saved
# tab order, and the sprint tab's (where the board is served, which events go to it, the script nexus adds to it, and
# how its scroll glides).
add_library(nexus_window_logic STATIC src/friends.cpp src/sprint.cpp src/scroll_glide.cpp src/tab_order.cpp)
target_include_directories(nexus_window_logic PUBLIC src)
target_compile_options(nexus_window_logic PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_window_logic PUBLIC nexus_model prax::app prax::json prax::process prax::core_cpp)

# The window's tree over Prax's UI context and Studio Kit: built by the app, and headless by tests/window/.
add_library(nexus_view STATIC src/view.cpp)
target_include_directories(nexus_view PUBLIC src)
target_compile_options(nexus_view PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_view PUBLIC nexus_window_logic
    prax::studio_kit prax::ui prax::ui_cpp prax::layout prax::font prax::text prax::draw prax::app prax::core_cpp)

# The terminal tabs' shells over Prax's terminal (view.h Terminals): built by the app, and run by the functional test.
add_library(nexus_terminals STATIC src/terminals.cpp)
target_include_directories(nexus_terminals PUBLIC src)
target_compile_options(nexus_terminals PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_terminals PUBLIC nexus_view prax::studio_terminal prax::process prax::vterm prax::core prax::core_cpp)

# The sprint tab's page over Prax's HTML view, scripts and network (view.h Board): built by the app, and run by the
# functional test on pages it serves from memory.
add_library(nexus_board STATIC src/board.cpp)
target_include_directories(nexus_board PUBLIC src)
target_compile_options(nexus_board PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_board PUBLIC nexus_view prax::html prax::html_quickjs prax::svg_quickjs prax::url_quickjs
    prax::dom prax::fetch prax::net prax::tls prax::http prax::image prax::compress prax::text prax::font prax::draw
    prax::layout prax::ui prax::studio_kit prax::app prax::core_cpp)

if(NEXUS_LINK)
    # The link over mosh's transport: its sessions (both ends), the ssh bootstrap, and the window's link thread.
    add_library(nexus_link STATIC src/link/mosh_link.cpp src/link/bootstrap.cpp src/link/client.cpp)
    target_include_directories(nexus_link PUBLIC src)
    target_compile_options(nexus_link PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus_link PUBLIC nexus_sync nexus_window_logic mosh_transport)

    # The server the window talks to with --server: headless, on the machine that keeps going.
    add_executable(nexus-server src/server/main.cpp src/server/server.cpp)
    target_compile_options(nexus-server PRIVATE ${NEXUS_WARNINGS})
    target_link_libraries(nexus-server PRIVATE nexus_link nexus_durable)
endif()

# The app, as a real bundle: nexus.app.
add_executable(nexus MACOSX_BUNDLE src/main.cpp src/platform_mac.mm)
set_source_files_properties(src/platform_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_options(nexus PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus PRIVATE nexus_view nexus_terminals nexus_board
    prax::studio_app prax::gpu prax::image prax::compress
    "-framework AppKit" "-framework Carbon" "-framework QuartzCore")
if(NEXUS_LINK)
    target_compile_definitions(nexus PRIVATE NEXUS_LINK=1)
    target_link_libraries(nexus PRIVATE nexus_link nexus_sync)
endif()
set_target_properties(nexus PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.robotgamedevelopers.nexus
    MACOSX_BUNDLE_BUNDLE_NAME nexus
    MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1.0
    MACOSX_BUNDLE_BUNDLE_VERSION 0.1.0)

enable_testing()
add_subdirectory(tests/model)
add_subdirectory(tests/sync)
add_subdirectory(tests/window)
add_subdirectory(tests/store)
if(NEXUS_LINK)
    add_subdirectory(tests/link)
endif()
