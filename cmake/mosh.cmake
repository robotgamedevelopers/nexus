# mosh's network layer (github.com/mobile-shell/mosh, GPL v3 or later with an OpenSSL exception), as the transport
# between nexus and nexus-server: its State Synchronization Protocol over UDP with AES-128-OCB, roaming, heartbeats,
# pacing, fragmentation and diff-based retransmission. Pinned to a release tag, compiled from its own sources unmodified.
# GPL v3 section 13 lets it be combined with nexus's AGPL v3; its notices stay in its files (see README.md, Licence).
#
# Only what Network::Transport needs is built: src/network, src/crypto with mosh's own OCB over the platform's AES (on
# macOS Apple's CommonCrypto, its `--with-crypto-library=apple-common-crypto`, no library to install; elsewhere OpenSSL's
# libcrypto, its `--with-crypto-library=openssl`), src/util/timestamp.cc, and the Instruction protobuf it sends. mosh is built with autotools; its config.h is generated here from the same checks its configure.ac
# makes (cmake/mosh_config.h.in).
#
# Needs protobuf (its CMake package and protoc) and zlib; and OpenSSL's libcrypto off macOS. On macOS: `brew install
# protobuf`; zlib is in the SDK. On Ubuntu: `apt install libprotobuf-dev protobuf-compiler zlib1g-dev libssl-dev`.

include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)
include(CheckSymbolExists)

FetchContent_Declare(mosh
    GIT_REPOSITORY https://github.com/mobile-shell/mosh.git
    GIT_TAG bc73a26316ede2a79259d859f8ee309b32412420   # mosh-1.4.0
    EXCLUDE_FROM_ALL)
# mosh has no CMakeLists.txt, so this only fetches it; its sources are compiled by the target below.
FetchContent_MakeAvailable(mosh)

find_package(ZLIB REQUIRED)
# iOS builds protobuf's lite runtime and takes a pinned protoc (cmake/ios_link.cmake): nothing to find on the machine.
if(NOT NEXUS_IOS)
# protobuf's own CMake package where it is installed (Homebrew's); else CMake's FindProtobuf, for distributions that
# ship protobuf without that package (Ubuntu's libprotobuf-dev), which gives the same protobuf::libprotobuf-lite target
# and protobuf_generate.
find_package(protobuf CONFIG QUIET)
if(NOT protobuf_FOUND)
    find_package(Protobuf MODULE QUIET)
endif()
if(NOT protobuf_FOUND AND NOT Protobuf_FOUND)
    message(FATAL_ERROR "nexus: protobuf was not found, and mosh's transport needs it (with protoc). "
        "Install it (on macOS: brew install protobuf; on Ubuntu: apt install libprotobuf-dev protobuf-compiler), or "
        "point CMAKE_PREFIX_PATH at it; or configure with -DNEXUS_LINK=OFF to build without nexus-server and the "
        "client's --server.")
endif()

# A protobuf library built for a newer macOS than CMAKE_OSX_DEPLOYMENT_TARGET (Homebrew builds for the macOS it runs
# on) makes whatever links it need that newer macOS: nexus's own targets, made after this point, are built for it, so
# the linker has nothing to warn about and the build says plainly what it runs on. Prax, made before, keeps its own.
get_target_property(protobuf_lite protobuf::libprotobuf-lite IMPORTED_LOCATION_RELEASE)
if(NOT protobuf_lite)
    get_target_property(protobuf_lite protobuf::libprotobuf-lite LOCATION)
endif()
find_program(VTOOL vtool)
if(VTOOL AND protobuf_lite)
    execute_process(COMMAND ${VTOOL} -show-build ${protobuf_lite} OUTPUT_VARIABLE protobuf_build ERROR_QUIET)
    string(REGEX MATCH "minos ([0-9.]+)" _ "${protobuf_build}")
    set(protobuf_minos "${CMAKE_MATCH_1}")
    if(protobuf_minos AND protobuf_minos VERSION_GREATER CMAKE_OSX_DEPLOYMENT_TARGET)
        message(STATUS "nexus: ${protobuf_lite} is built for macOS ${protobuf_minos}, so nexus and nexus-server are too")
        set(CMAKE_OSX_DEPLOYMENT_TARGET ${protobuf_minos})
    endif()
endif()
endif() # NOT NEXUS_IOS

# config.h, from the checks mosh's configure.ac makes for the files built here.
check_include_file_cxx(sys/uio.h HAVE_SYS_UIO_H)
check_include_file_cxx(endian.h HAVE_ENDIAN_H)
check_include_file_cxx(sys/endian.h HAVE_SYS_ENDIAN_H)
check_include_file_cxx(strings.h HAVE_STRINGS_H)
if(APPLE)
    check_include_file_cxx(CommonCrypto/CommonCrypto.h HAVE_COMMONCRYPTO)
    if(NOT HAVE_COMMONCRYPTO)
        message(FATAL_ERROR "nexus: mosh's crypto is built over Apple's CommonCrypto here, and its header was not found.")
    endif()
    set(USE_APPLE_COMMON_CRYPTO_AES ON)
else()
    find_package(OpenSSL 3 COMPONENTS Crypto)
    if(NOT OpenSSL_FOUND)
        message(FATAL_ERROR "nexus: mosh's crypto is built over OpenSSL's libcrypto here, and it was not found. Install "
            "it (on Ubuntu: apt install libssl-dev), or configure with -DNEXUS_LINK=OFF.")
    endif()
    set(USE_OPENSSL_AES ON)
endif()
check_symbol_exists(IP_MTU_DISCOVER netinet/in.h HAVE_IP_MTU_DISCOVER)
check_symbol_exists(IP_RECVTOS netinet/in.h HAVE_IP_RECVTOS)
check_symbol_exists(clock_gettime time.h HAVE_CLOCK_GETTIME)
check_symbol_exists(mach_absolute_time mach/mach_time.h HAVE_MACH_ABSOLUTE_TIME)
check_symbol_exists(gettimeofday sys/time.h HAVE_GETTIMEOFDAY)
check_symbol_exists(posix_memalign stdlib.h HAVE_POSIX_MEMALIGN)
check_symbol_exists(OSSwapHostToBigInt64 libkern/OSByteOrder.h HAVE_OSX_SWAP)
check_symbol_exists(ffs strings.h HAVE_DECL_FFS)
set(endian_headers)
if(HAVE_ENDIAN_H)
    list(APPEND endian_headers endian.h)
elseif(HAVE_SYS_ENDIAN_H)
    list(APPEND endian_headers sys/types.h sys/endian.h)
else()
    list(APPEND endian_headers stdint.h)
endif()
check_symbol_exists(be64toh "${endian_headers}" HAVE_DECL_BE64TOH)
check_symbol_exists(betoh64 "${endian_headers}" HAVE_DECL_BETOH64)
check_symbol_exists(bswap64 "${endian_headers}" HAVE_DECL_BSWAP64)
check_cxx_source_compiles("int main() { return __builtin_ctz(2) == 1 ? 0 : 1; }" HAVE_DECL___BUILTIN_CTZ)
check_cxx_source_compiles("int main() { return __builtin_bswap64(1) != 0 ? 0 : 1; }" HAVE_DECL___BUILTIN_BSWAP64)
foreach(decl HAVE_DECL_FFS HAVE_DECL_BE64TOH HAVE_DECL_BETOH64 HAVE_DECL_BSWAP64 HAVE_DECL___BUILTIN_CTZ
        HAVE_DECL___BUILTIN_BSWAP64)
    if(${decl})
        set(${decl} 1)
    else()
        set(${decl} 0)
    endif()
endforeach()
set(mosh_config_dir "${CMAKE_CURRENT_BINARY_DIR}/mosh-config")
configure_file("${CMAKE_CURRENT_LIST_DIR}/mosh_config.h.in" "${mosh_config_dir}/config.h")

# The Instruction message mosh's sender and receiver exchange.
set(mosh_proto_dir "${CMAKE_CURRENT_BINARY_DIR}/mosh-protobufs")
file(MAKE_DIRECTORY "${mosh_proto_dir}")
add_library(mosh_transport STATIC
    ${mosh_SOURCE_DIR}/src/protobufs/transportinstruction.proto
    ${mosh_SOURCE_DIR}/src/network/network.cc
    ${mosh_SOURCE_DIR}/src/network/transportfragment.cc
    ${mosh_SOURCE_DIR}/src/network/compressor.cc
    ${mosh_SOURCE_DIR}/src/crypto/crypto.cc
    ${mosh_SOURCE_DIR}/src/crypto/base64.cc
    ${mosh_SOURCE_DIR}/src/crypto/ocb_internal.cc
    ${mosh_SOURCE_DIR}/src/util/timestamp.cc)
if(NEXUS_IOS)
    # protoc 21.12 for the building Mac, the generator of the runtime built for iOS (cmake/ios_link.cmake).
    add_custom_command(OUTPUT "${mosh_proto_dir}/transportinstruction.pb.cc" "${mosh_proto_dir}/transportinstruction.pb.h"
        COMMAND "${NEXUS_PROTOC}" --cpp_out=${mosh_proto_dir} -I${mosh_SOURCE_DIR}/src/protobufs
            ${mosh_SOURCE_DIR}/src/protobufs/transportinstruction.proto
        DEPENDS ${mosh_SOURCE_DIR}/src/protobufs/transportinstruction.proto
        VERBATIM)
    target_sources(mosh_transport PRIVATE "${mosh_proto_dir}/transportinstruction.pb.cc")
else()
protobuf_generate(TARGET mosh_transport LANGUAGE cpp
    IMPORT_DIRS ${mosh_SOURCE_DIR}/src/protobufs
    PROTOC_OUT_DIR "${mosh_proto_dir}")
endif()
# SYSTEM: the transport's templates (networktransport-impl.h, transportsender-impl.h) are instantiated in nexus's own
# translation units, which compile with nexus's warnings as errors; mosh's headers are not ours to change.
target_include_directories(mosh_transport SYSTEM PUBLIC
    ${mosh_config_dir}
    ${mosh_proto_dir}
    ${mosh_SOURCE_DIR}/src/network
    ${mosh_SOURCE_DIR}/src/crypto
    ${mosh_SOURCE_DIR}/src/util)
# mosh's own sources build with no warning flags of nexus's (they are not ours to change); -w silences the warnings
# that clang raises on them by default (deprecated declarations among them), so the build log stays clean.
target_compile_options(mosh_transport PRIVATE -w)
target_link_libraries(mosh_transport PUBLIC protobuf::libprotobuf-lite ZLIB::ZLIB)
if(USE_OPENSSL_AES)
    target_link_libraries(mosh_transport PUBLIC OpenSSL::Crypto)
endif()
# The generated message code is built as the protobuf library it calls was: release, without sanitizers. Built without
# NDEBUG, or with a sanitizer, protobuf's headers switch on a test-only check (DebugHardenCheckHasBitConsistency in
# protobuf's port.h) that calls into the library, and a release library (Homebrew's) faults in it on the first message
# copied: every Debug or sanitizer build of the link crashed there. The rest of mosh and of nexus keep the build's flags.
set_source_files_properties("${mosh_proto_dir}/transportinstruction.pb.cc" TARGET_DIRECTORY mosh_transport
    PROPERTIES COMPILE_DEFINITIONS NDEBUG COMPILE_OPTIONS "-fno-sanitize=all")
