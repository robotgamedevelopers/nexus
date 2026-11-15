# The Tailscale shim (src/tailscale, docs/tailscale.md): the Go code over tsnet, built with the pinned Go toolchain into
#   macOS          a c-archive, libnts.a, one per architecture in CMAKE_OSX_ARCHITECTURES (arm64, x86_64, or both, then
#                  joined by lipo); the host's architecture when it is empty
#   Linux          a c-archive, libnts.a (x86_64, inside WSL2 too)
#   Windows        a DLL, nts.dll, with an MSVC import library, nts.lib, made from src/tailscale/nts.def by MSVC's lib;
#                  cgo needs a gcc-style compiler, so the DLL's C parts are compiled by llvm-mingw (pinned below,
#                  fetched into NEXUS_TAILSCALE_DOWNLOADS unless NEXUS_LLVM_MINGW names one)
# and the imported target nexus_tailscale over it. A program links it with nexus_link_tailscale(<target>), which on
# Windows also embeds the manifest that declares Windows 10 (cmake/windows10.manifest: without it tsnet panics at start)
# and copies nts.dll beside the program.
#
# The build is reproducible: Go is told the exact toolchain (GOTOOLCHAIN=go<pin>), which any Go from 1.21 on fetches
# into the module cache and verifies against the checksum database; modules come only at the versions go.sum pins
# (-mod=readonly); no Go environment file is read (GOENV=off) and no workspace (GOWORK=off); paths are trimmed and no
# VCS stamp is taken. The caches are cache variables so that CI keeps them outside its workspace:
#   NEXUS_GO_MODCACHE        modules and the toolchain (default: <build>/go/mod)
#   NEXUS_GO_BUILDCACHE      Go's build cache (default: <build>/go/build)
#   NEXUS_TAILSCALE_DOWNLOADS  llvm-mingw on Windows (default: <build>/go/downloads)

set(NEXUS_TAILSCALE_GO 1.27.1)
set(NEXUS_TAILSCALE_MODULE_VERSION v1.104.1)
# llvm-mingw, UCRT, x86_64 (github.com/mstorsjo/llvm-mingw, release 20261020, LLVM 23.1.4).
set(NEXUS_LLVM_MINGW_RELEASE 20261020)
set(NEXUS_LLVM_MINGW_NAME llvm-mingw-${NEXUS_LLVM_MINGW_RELEASE}-ucrt-x86_64)
set(NEXUS_LLVM_MINGW_SHA256 fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd)

get_filename_component(nexus_ts_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(nexus_ts_source ${nexus_ts_root}/src/tailscale)
set(nexus_ts_out ${CMAKE_BINARY_DIR}/tailscale)
file(MAKE_DIRECTORY ${nexus_ts_out})

set(NEXUS_GO_MODCACHE "${CMAKE_BINARY_DIR}/go/mod" CACHE PATH "Go's module cache for the Tailscale shim")
set(NEXUS_GO_BUILDCACHE "${CMAKE_BINARY_DIR}/go/build" CACHE PATH "Go's build cache for the Tailscale shim")
set(NEXUS_TAILSCALE_DOWNLOADS "${CMAKE_BINARY_DIR}/go/downloads" CACHE PATH
    "Where the Tailscale shim's Windows C compiler (llvm-mingw) is fetched")
set(NEXUS_LLVM_MINGW "" CACHE PATH "An llvm-mingw ${NEXUS_LLVM_MINGW_RELEASE} directory (empty: fetch it)")

find_program(NEXUS_GO go HINTS "$ENV{HOME}/sdk/bin" "$ENV{ProgramFiles}/Go/bin")
if(NOT NEXUS_GO)
    message(FATAL_ERROR "nexus: the Tailscale shim needs Go (1.21 or later, which fetches the pinned go"
        "${NEXUS_TAILSCALE_GO} itself) on PATH or in ~/sdk/bin; install it, or configure with -DNEXUS_TAILSCALE=OFF")
endif()

if(APPLE)
    set(nexus_ts_archs ${CMAKE_OSX_ARCHITECTURES})
    if(NOT nexus_ts_archs)
        set(nexus_ts_archs ${CMAKE_HOST_SYSTEM_PROCESSOR})
    endif()
elseif(WIN32)
    set(nexus_ts_archs x86_64)
else()
    set(nexus_ts_archs ${CMAKE_SYSTEM_PROCESSOR})
endif()

# Go's environment, the same for every build here. CC is quoted wherever it is set below: Go splits CC on spaces,
# and a path may hold one (C:/Users/<name with a space>).
set(nexus_go_env GOTOOLCHAIN=go${NEXUS_TAILSCALE_GO} GOENV=off GOWORK=off GOFLAGS=-mod=readonly CGO_ENABLED=1
    GOMODCACHE=${NEXUS_GO_MODCACHE} GOCACHE=${NEXUS_GO_BUILDCACHE})
set(nexus_go_build ${NEXUS_GO} build -trimpath -buildvcs=false)
file(GLOB nexus_ts_go_sources CONFIGURE_DEPENDS ${nexus_ts_source}/shim/*.go)
set(nexus_ts_depends ${nexus_ts_go_sources} ${nexus_ts_source}/go.mod ${nexus_ts_source}/go.sum)

if(WIN32)
    if(NOT NEXUS_LLVM_MINGW)
        set(NEXUS_LLVM_MINGW "${NEXUS_TAILSCALE_DOWNLOADS}/${NEXUS_LLVM_MINGW_NAME}")
    endif()
    set(nexus_ts_cc "${NEXUS_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang.exe")
    if(NOT EXISTS "${nexus_ts_cc}")
        # Downloaded and unpacked beside its final place, checked, then renamed into it, so that a fetch cut short
        # leaves nothing half made.
        set(zip "${NEXUS_TAILSCALE_DOWNLOADS}/${NEXUS_LLVM_MINGW_NAME}.zip")
        set(partial "${NEXUS_TAILSCALE_DOWNLOADS}/${NEXUS_LLVM_MINGW_NAME}.partial")
        file(MAKE_DIRECTORY "${NEXUS_TAILSCALE_DOWNLOADS}")
        message(STATUS "nexus: fetching ${NEXUS_LLVM_MINGW_NAME} (cgo's C compiler for the Tailscale shim's DLL)")
        file(DOWNLOAD
            "https://github.com/mstorsjo/llvm-mingw/releases/download/${NEXUS_LLVM_MINGW_RELEASE}/${NEXUS_LLVM_MINGW_NAME}.zip"
            "${zip}" EXPECTED_HASH SHA256=${NEXUS_LLVM_MINGW_SHA256} STATUS fetched)
        list(GET fetched 0 fetched_code)
        if(NOT fetched_code EQUAL 0)
            file(REMOVE "${zip}")
            message(FATAL_ERROR "nexus: fetching ${NEXUS_LLVM_MINGW_NAME}.zip failed (${fetched}); configure again, "
                "or give an unpacked llvm-mingw ${NEXUS_LLVM_MINGW_RELEASE} with -DNEXUS_LLVM_MINGW=<dir>")
        endif()
        file(REMOVE_RECURSE "${partial}")
        file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${partial}")
        file(RENAME "${partial}/${NEXUS_LLVM_MINGW_NAME}" "${NEXUS_LLVM_MINGW}")
        file(REMOVE_RECURSE "${partial}")
        file(REMOVE "${zip}")
    endif()
    if(NOT EXISTS "${nexus_ts_cc}")
        message(FATAL_ERROR "nexus: ${nexus_ts_cc} is missing: NEXUS_LLVM_MINGW (${NEXUS_LLVM_MINGW}) is not an "
            "unpacked ${NEXUS_LLVM_MINGW_NAME}")
    endif()
    if(NOT CMAKE_AR)
        message(FATAL_ERROR "nexus: no MSVC lib.exe (CMAKE_AR) to make the shim's import library")
    endif()

    set(nexus_ts_dll ${nexus_ts_out}/nts.dll)
    set(nexus_ts_implib ${nexus_ts_out}/nts.lib)
    add_custom_command(OUTPUT ${nexus_ts_dll} ${nexus_ts_implib}
        COMMAND ${CMAKE_COMMAND} -E env ${nexus_go_env} GOOS=windows GOARCH=amd64 "CC=\"${nexus_ts_cc}\""
            ${nexus_go_build} -buildmode=c-shared "-ldflags=-s -w" -o ${nexus_ts_dll} ./shim
        COMMAND ${CMAKE_AR} /nologo /def:${nexus_ts_source}/nts.def /machine:x64 /out:${nexus_ts_implib}
        DEPENDS ${nexus_ts_depends} ${nexus_ts_source}/nts.def
        WORKING_DIRECTORY ${nexus_ts_source}
        COMMENT "Building the Tailscale shim (nts.dll and its import library) with go${NEXUS_TAILSCALE_GO}"
        VERBATIM)
    add_custom_target(nexus_tailscale_shim DEPENDS ${nexus_ts_dll} ${nexus_ts_implib})
    add_library(nexus_tailscale SHARED IMPORTED GLOBAL)
    set_target_properties(nexus_tailscale PROPERTIES IMPORTED_LOCATION ${nexus_ts_dll}
        IMPORTED_IMPLIB ${nexus_ts_implib} INTERFACE_INCLUDE_DIRECTORIES ${nexus_ts_source})
else()
    set(nexus_ts_archives)
    foreach(arch ${nexus_ts_archs})
        if(arch STREQUAL "arm64" OR arch STREQUAL "aarch64")
            set(goarch arm64)
        elseif(arch STREQUAL "x86_64" OR arch STREQUAL "amd64")
            set(goarch amd64)
        else()
            message(FATAL_ERROR "nexus: the Tailscale shim builds for arm64 and x86_64, not ${arch}")
        endif()
        set(flags "")
        if(APPLE)
            # The architecture, and the oldest macOS the build runs on, so that the linker never meets an object
            # built for a newer one.
            set(flags "-arch ${arch} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
            if(CMAKE_OSX_SYSROOT)
                string(APPEND flags " -isysroot ${CMAKE_OSX_SYSROOT}")
            endif()
            set(goos darwin)
        else()
            set(goos linux)
        endif()
        set(archive ${nexus_ts_out}/libnts-${arch}.a)
        add_custom_command(OUTPUT ${archive}
            COMMAND ${CMAKE_COMMAND} -E env ${nexus_go_env} GOOS=${goos} GOARCH=${goarch} "CC=\"${CMAKE_C_COMPILER}\""
                "CGO_CFLAGS=-O2 ${flags}" "CGO_LDFLAGS=${flags}"
                ${nexus_go_build} -buildmode=c-archive -o ${archive} ./shim
            DEPENDS ${nexus_ts_depends}
            WORKING_DIRECTORY ${nexus_ts_source}
            COMMENT "Building the Tailscale shim (libnts.a, ${arch}) with go${NEXUS_TAILSCALE_GO}"
            VERBATIM)
        list(APPEND nexus_ts_archives ${archive})
    endforeach()
    set(nexus_ts_archive ${nexus_ts_out}/libnts.a)
    list(LENGTH nexus_ts_archives nexus_ts_count)
    if(nexus_ts_count GREATER 1)
        add_custom_command(OUTPUT ${nexus_ts_archive}
            COMMAND lipo -create ${nexus_ts_archives} -output ${nexus_ts_archive}
            DEPENDS ${nexus_ts_archives}
            COMMENT "Joining the Tailscale shim's architectures (lipo)"
            VERBATIM)
    else()
        add_custom_command(OUTPUT ${nexus_ts_archive}
            COMMAND ${CMAKE_COMMAND} -E copy ${nexus_ts_archives} ${nexus_ts_archive}
            DEPENDS ${nexus_ts_archives}
            VERBATIM)
    endif()
    add_custom_target(nexus_tailscale_shim DEPENDS ${nexus_ts_archive})
    add_library(nexus_tailscale STATIC IMPORTED GLOBAL)
    set_target_properties(nexus_tailscale PROPERTIES IMPORTED_LOCATION ${nexus_ts_archive}
        INTERFACE_INCLUDE_DIRECTORIES ${nexus_ts_source})
    find_package(Threads REQUIRED)
    if(APPLE)
        # What the Go runtime and tsnet call on macOS.
        set_property(TARGET nexus_tailscale PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads resolv
            "-framework CoreFoundation" "-framework Security" "-framework SystemConfiguration" "-framework IOKit")
    else()
        set_property(TARGET nexus_tailscale PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS} resolv)
    endif()
endif()

# Links the shim into target: the library, the build order, and on Windows the manifest and nts.dll beside the program.
function(nexus_link_tailscale target)
    target_link_libraries(${target} PRIVATE nexus_tailscale)
    add_dependencies(${target} nexus_tailscale_shim)
    if(WIN32)
        target_sources(${target} PRIVATE ${nexus_ts_root}/cmake/windows10.manifest)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${nexus_ts_dll} $<TARGET_FILE_DIR:${target}>
            VERBATIM)
    endif()
endfunction()
