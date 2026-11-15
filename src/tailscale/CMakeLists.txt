# The Tailscale shim and its tests on their own, without Prax or the rest of nexus, so that CI's tailscale jobs
# configure in seconds:
#   cmake -S src/tailscale -B build-tailscale -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-tailscale --target tailscale_test && ctest --test-dir build-tailscale -L tailscale
# The top-level CMakeLists.txt builds the same (cmake/tailscale.cmake, tests/tailscale) with -DNEXUS_TAILSCALE=ON.
cmake_minimum_required(VERSION 3.28)

# As the top-level CMakeLists.txt: the oldest macOS nexus runs on, set before project(), which reads it.
set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "Oldest macOS the build runs on")

project(nexus_tailscale LANGUAGES C CXX)
if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "nexus: Windows builds are 64-bit only; configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The top-level CMakeLists.txt's warnings, as errors.
if(MSVC)
    set(NEXUS_WARNINGS /W4 /WX)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
    set(NEXUS_WARNINGS -Wall -Wextra -Wpedantic -Werror)
endif()

get_filename_component(nexus_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include(${nexus_root}/cmake/tailscale.cmake)

enable_testing()
add_subdirectory(${nexus_root}/tests/tailscale ${CMAKE_BINARY_DIR}/tests/tailscale)
