# nexus: tracer bullet for the first window. macOS only for now.
cmake_minimum_required(VERSION 3.28)

# Prax runs on macOS 26 or newer; set before project(), which reads it.
set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "Oldest macOS the build runs on")

project(nexus LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

# Prax options: leave out what this window does not use.
set(PR_JS OFF CACHE BOOL "" FORCE)
set(PR_CHARSET_LEGACY OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(prax
    GIT_REPOSITORY https://github.com/pdoane/prax.git
    GIT_TAG 73edfdb9b741307b31fc7fdff1e757860aa7abea
    # SYSTEM: Prax's headers use Clang nullability, which -Wpedantic rejects in our code otherwise.
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(prax)

# The non-drawing logic: tabs, transcripts, prompt text, send and queue. No OS, no time.
add_library(nexus_model STATIC src/model.cpp)
target_include_directories(nexus_model PUBLIC src)
target_compile_options(nexus_model PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(nexus src/main.cpp src/view.cpp src/friends.cpp)
target_compile_options(nexus PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nexus PRIVATE nexus_model
    prax::app prax::draw prax::gpu prax::font prax::text prax::image prax::compress prax::core_cpp)
