# The link's own libraries for iOS and iPadOS (included from cmake/ios.cmake): what the desktop build takes from the
# machine (Homebrew's protobuf and protoc, the system's ssh) the iOS build compiles itself, each a pinned release,
# checked by its SHA-256 and compiled from its own sources unmodified.
#
# - protobuf's lite runtime (protobuf 3.21.12, BSD-3-Clause), for mosh's Instruction message (cmake/mosh.cmake), with
#   protoc 21.12 for the Mac that builds, which generates code for that runtime.
# - mbedTLS 3.6.8 (Apache-2.0), the crypto under libssh2.
# - libssh2 1.11.1 (BSD-3-Clause) over mbedTLS, for the in-app ssh that runs `nexus-server new` (src/link/inapp_ssh.h):
#   an iOS app cannot run /usr/bin/ssh.
#
# Each builds with its own flags and -w: they are not ours to change, and nexus's warnings-as-errors cover nexus's code.

include(FetchContent)

# protoc runs on the Mac that builds, so it is the release built for macOS on Apple silicon (the arm64 build machines
# cmake/ios.cmake requires), not one compiled here for iOS.
set(nexus_protoc_dir "${CMAKE_BINARY_DIR}/protoc-21.12")
set(nexus_protoc_zip "${nexus_protoc_dir}.zip")
if(NOT EXISTS "${nexus_protoc_dir}/bin/protoc")
    file(DOWNLOAD https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protoc-21.12-osx-aarch_64.zip
        "${nexus_protoc_zip}"
        EXPECTED_HASH SHA256=96839af0caed64352442fc8236f4bdf7c1cd6efcfaa98fa5db37307a73fc7c70
        STATUS protoc_status)
    list(GET protoc_status 0 protoc_code)
    if(NOT protoc_code EQUAL 0)
        message(FATAL_ERROR "nexus: downloading protoc 21.12 for the iOS build failed: ${protoc_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${nexus_protoc_zip}" DESTINATION "${nexus_protoc_dir}")
    file(REMOVE "${nexus_protoc_zip}")
endif()
set(NEXUS_PROTOC "${nexus_protoc_dir}/bin/protoc")

FetchContent_Declare(nexus_protobuf
    URL https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protobuf-cpp-3.21.12.tar.gz
    URL_HASH SHA256=4eab9b524aa5913c6fffb20b2a8abf5ef7f95a80bc0701f3a6dbb4c607f73460
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBPROTOC OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(nexus_mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.8/mbedtls-3.6.8.tar.bz2
    URL_HASH SHA256=bc54b818e87f49c0b3281dd85d24adaede0e63b4e822e3fbd49c005a38b9595b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)

# libssh2 is compiled from its sources here (its CMake looks for mbedTLS installed, not built beside it).
FetchContent_Declare(nexus_libssh2
    URL https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz
    URL_HASH SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-here)

# Their own CMake is written for older CMake policies, which this CMake warns of; they are not ours to change.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(nexus_protobuf nexus_mbedtls nexus_libssh2)
unset(CMAKE_POLICY_VERSION_MINIMUM)

foreach(lib libprotobuf-lite mbedcrypto mbedx509 mbedtls)
    target_compile_options(${lib} PRIVATE -w)
endforeach()
if(NOT TARGET protobuf::libprotobuf-lite)
    add_library(protobuf::libprotobuf-lite ALIAS libprotobuf-lite)
endif()

set(nexus_libssh2_src "${nexus_libssh2_SOURCE_DIR}/src")
add_library(nexus_libssh2 STATIC
    ${nexus_libssh2_src}/agent.c ${nexus_libssh2_src}/bcrypt_pbkdf.c ${nexus_libssh2_src}/channel.c
    ${nexus_libssh2_src}/comp.c ${nexus_libssh2_src}/chacha.c ${nexus_libssh2_src}/cipher-chachapoly.c
    ${nexus_libssh2_src}/crypt.c ${nexus_libssh2_src}/crypto.c ${nexus_libssh2_src}/global.c
    ${nexus_libssh2_src}/hostkey.c ${nexus_libssh2_src}/keepalive.c ${nexus_libssh2_src}/kex.c
    ${nexus_libssh2_src}/knownhost.c ${nexus_libssh2_src}/mac.c ${nexus_libssh2_src}/misc.c
    ${nexus_libssh2_src}/packet.c ${nexus_libssh2_src}/pem.c ${nexus_libssh2_src}/poly1305.c
    ${nexus_libssh2_src}/publickey.c ${nexus_libssh2_src}/scp.c ${nexus_libssh2_src}/session.c
    ${nexus_libssh2_src}/sftp.c ${nexus_libssh2_src}/transport.c ${nexus_libssh2_src}/userauth.c
    ${nexus_libssh2_src}/userauth_kbd_packet.c ${nexus_libssh2_src}/version.c)
# What libssh2's own configure finds on iOS (its libssh2_config_cmake.h.in), stated rather than probed: the SDK has
# each of these.
target_compile_definitions(nexus_libssh2 PRIVATE LIBSSH2_MBEDTLS HAVE_UNISTD_H HAVE_INTTYPES_H HAVE_SYS_SELECT_H
    HAVE_SYS_UIO_H HAVE_SYS_SOCKET_H HAVE_SYS_IOCTL_H HAVE_SYS_TIME_H HAVE_SYS_UN_H HAVE_ARPA_INET_H HAVE_NETINET_IN_H
    HAVE_GETTIMEOFDAY HAVE_STRTOLL HAVE_SNPRINTF HAVE_MEMSET_S HAVE_POLL HAVE_SELECT HAVE_O_NONBLOCK)
target_include_directories(nexus_libssh2 SYSTEM PUBLIC "${nexus_libssh2_SOURCE_DIR}/include")
target_compile_options(nexus_libssh2 PRIVATE -w)
target_link_libraries(nexus_libssh2 PUBLIC mbedcrypto)
