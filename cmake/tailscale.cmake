# The Tailscale shim (src/tailscale, docs/tailscale.md): the Go code over tsnet, built with the pinned Go toolchain into
#   macOS          a c-archive, libnts.a, one per architecture in CMAKE_OSX_ARCHITECTURES (arm64, x86_64, or both, then
#                  joined by lipo); the host's architecture when it is empty
#   Linux          a c-archive, libnts.a (x86_64, inside WSL2 too)
#   Windows        a DLL, nts.dll, with an MSVC import library, nts.lib, made from src/tailscale/nts.def by MSVC's lib;
#                  cgo needs a gcc-style compiler, so the DLL's C parts are compiled by llvm-mingw (pinned in src/tailscale/pins.cmake,
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

get_filename_component(nexus_ts_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include(${nexus_ts_root}/src/tailscale/pins.cmake)
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
    # A fetched llvm-mingw holds the sha256 of the archive it came from (nexus-zip.sha256, written before the rename
    # that puts it in place); one whose stamp is not the pin's is fetched again. One named by NEXUS_LLVM_MINGW is used
    # as it is.
    set(nexus_ts_fetch FALSE)
    if(NOT NEXUS_LLVM_MINGW)
        set(nexus_ts_fetch TRUE)
        set(NEXUS_LLVM_MINGW "${NEXUS_TAILSCALE_DOWNLOADS}/${NEXUS_LLVM_MINGW_NAME}")
    endif()
    set(nexus_ts_cc "${NEXUS_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang.exe")
    set(nexus_ts_stamp "${NEXUS_LLVM_MINGW}/nexus-zip.sha256")
    if(nexus_ts_fetch AND EXISTS "${NEXUS_LLVM_MINGW}")
        set(stamped "")
        if(EXISTS "${nexus_ts_stamp}")
            file(READ "${nexus_ts_stamp}" stamped)
            string(STRIP "${stamped}" stamped)
        endif()
        if(NOT stamped STREQUAL NEXUS_LLVM_MINGW_SHA256 OR NOT EXISTS "${nexus_ts_cc}")
            message(STATUS "nexus: ${NEXUS_LLVM_MINGW} is not the pinned ${NEXUS_LLVM_MINGW_NAME}.zip: fetching it again")
            file(REMOVE_RECURSE "${NEXUS_LLVM_MINGW}")
        endif()
    endif()
    set(renamed "")
    if(nexus_ts_fetch AND NOT EXISTS "${nexus_ts_cc}")
        # Downloaded and unpacked beside its final place under a name of this configure's own, checked, then renamed
        # into it, so that a fetch cut short leaves nothing half made, and configures sharing NEXUS_TAILSCALE_DOWNLOADS
        # (CI's runners on one machine) may fetch at once: the first rename wins, and the others find it in place.
        string(RANDOM LENGTH 12 fetch_id)
        set(zip "${NEXUS_TAILSCALE_DOWNLOADS}/${NEXUS_LLVM_MINGW_NAME}.${fetch_id}.zip")
        set(partial "${NEXUS_TAILSCALE_DOWNLOADS}/${NEXUS_LLVM_MINGW_NAME}.${fetch_id}.partial")
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
        file(WRITE "${partial}/${NEXUS_LLVM_MINGW_NAME}/nexus-zip.sha256" "${NEXUS_LLVM_MINGW_SHA256}\n")
        file(RENAME "${partial}/${NEXUS_LLVM_MINGW_NAME}" "${NEXUS_LLVM_MINGW}" RESULT renamed)
        file(REMOVE_RECURSE "${partial}")
        file(REMOVE "${zip}")
    endif()
    if(NOT EXISTS "${nexus_ts_cc}")
        message(FATAL_ERROR "nexus: ${nexus_ts_cc} is missing: NEXUS_LLVM_MINGW (${NEXUS_LLVM_MINGW}) is not an "
            "unpacked ${NEXUS_LLVM_MINGW_NAME} (the last rename into it: ${renamed})")
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
    set(nexus_ts_host_env GOOS=windows GOARCH=amd64 "CC=\"${nexus_ts_cc}\"")
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
        if(NOT nexus_ts_host_env)
            set(nexus_ts_host_env GOOS=${goos} GOARCH=${goarch} "CC=\"${CMAKE_C_COMPILER}\"" "CGO_CFLAGS=-O2 ${flags}"
                "CGO_LDFLAGS=${flags}")
        endif()
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
        # What the shim's packages ask the linker for (go list -deps -f '{{.CgoLDFLAGS}}' ./shim): runtime/cgo threads,
        # net the resolver, and github.com/tailscale/certstore two frameworks.
        set_property(TARGET nexus_tailscale PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads resolv
            "-framework CoreFoundation" "-framework Security")
    else()
        # runtime/cgo's threads (and dl, where the C library keeps it apart).
        set_property(TARGET nexus_tailscale PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})
    endif()
endif()

# go vet over the shim and the notices program, for the first architecture built here (CI runs it on every OS); and
# the check that src/tailscale/NOTICES.md is what the notices program writes at these pins (CI runs it on Linux).
add_custom_target(nexus_tailscale_vet
    COMMAND ${CMAKE_COMMAND} -E env ${nexus_go_env} ${nexus_ts_host_env} ${NEXUS_GO} vet ./...
    WORKING_DIRECTORY ${nexus_ts_source}
    COMMENT "go vet of the Tailscale shim with go${NEXUS_TAILSCALE_GO}"
    VERBATIM)
add_custom_target(nexus_tailscale_notices_check
    COMMAND ${CMAKE_COMMAND} -E env ${nexus_go_env} ${NEXUS_GO} run ./notices -check -go ${NEXUS_GO}
    WORKING_DIRECTORY ${nexus_ts_source}
    COMMENT "src/tailscale/NOTICES.md against the modules the shim links"
    VERBATIM)

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
