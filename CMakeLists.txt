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

# The non-drawing logic: tabs, transcripts, drafts, send and queue. No OS, no time.
add_library(nexus_model STATIC src/model.cpp)
target_include_directories(nexus_model PUBLIC src)
target_compile_options(nexus_model PRIVATE ${NEXUS_WARNINGS})

# The window's logic that runs without a window: the friend and machine lists' parsing (and the command that prints
# them), a terminal tab's label in ssh (remote.h), the saved tab order, and the sprint tab's (where the board is served,
# which events go to it, the script nexus adds to it).
add_library(nexus_window_logic STATIC src/friends.cpp src/remote.cpp src/sprint.cpp src/tab_order.cpp)
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
add_library(nexus_terminals STATIC src/terminals.cpp src/foreground.cpp)
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

# The app, as a real bundle: nexus.app.
add_executable(nexus MACOSX_BUNDLE src/main.cpp src/platform_mac.mm)
set_source_files_properties(src/platform_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_options(nexus PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus PRIVATE nexus_view nexus_terminals nexus_board
    prax::studio_app prax::gpu prax::image prax::compress
    "-framework AppKit" "-framework Carbon" "-framework QuartzCore")
set_target_properties(nexus PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.robotgamedevelopers.nexus
    MACOSX_BUNDLE_BUNDLE_NAME nexus
    MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1.0
    MACOSX_BUNDLE_BUNDLE_VERSION 0.1.0)

enable_testing()
add_subdirectory(tests/model)
add_subdirectory(tests/window)
