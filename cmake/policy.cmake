# Both production hosts embed the same Starlark interpreter. The CLI uses it to preview and apply a fork's rules.
find_program(NEXUS_CARGO cargo HINTS "$ENV{HOME}/sdk/bin" "$ENV{USERPROFILE}/sdk/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
set(policy_crate "${PROJECT_SOURCE_DIR}/policy/native")
set(policy_target "${CMAKE_BINARY_DIR}/policy-rust")
# A cross build (iOS: cmake/ios.cmake) sets NEXUS_RUST_TARGET: the archive the window links is built for that target,
# into Cargo's directory of its own, and the CLI for the machine that builds, as everywhere else.
set(policy_triple_dir "")
if(NEXUS_RUST_TARGET)
    set(policy_triple_dir "${NEXUS_RUST_TARGET}/")
endif()
set(policy_archive "${policy_target}/${policy_triple_dir}release/${CMAKE_STATIC_LIBRARY_PREFIX}nexus_policy_runtime${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(policy_tool "${CMAKE_BINARY_DIR}/nexus-policy${CMAKE_EXECUTABLE_SUFFIX}")
file(GLOB_RECURSE policy_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/policy/rules/*.star")
set(policy_cargo_env)
set(policy_cargo_target "${policy_target}")
set(policy_stage_archive)
# A runner handles one job at a time. Reuse compiled crates there and copy the final archive into this checkout.
if(DEFINED ENV{GITHUB_ACTIONS} AND DEFINED ENV{RUNNER_NAME})
    cmake_path(GET NEXUS_CARGO PARENT_PATH policy_cargo_bin)
    find_program(NEXUS_RUSTC rustc HINTS "${policy_cargo_bin}" REQUIRED)
    list(APPEND policy_cargo_env "RUSTC=${NEXUS_RUSTC}")
    # blake3's build script reruns when CC or CFLAGS changes, and every crate above it (pagable, starlark_map,
    # starlark_syntax, starlark) is rebuilt with it. ci.yml's build-linux sets CC=clang for the whole job, cache-warm
    # does not, so the cached target directory was stale for every pull request that touched the crate (58 s of a
    # 2-minute job, PR #131). Cargo runs without the job's C compiler settings, so its fingerprints match everywhere.
    list(PREPEND policy_cargo_env --unset=CC --unset=CXX --unset=CFLAGS --unset=CXXFLAGS)
    set(policy_cache_home "$ENV{HOME}")
    if(WIN32)
        set(policy_cache_home "$ENV{USERPROFILE}")
    endif()
    set(policy_cargo_target "${policy_cache_home}/.cache/nexus-ci/$ENV{RUNNER_NAME}/starlark")
    # GitHub-hosted runners have a new name for every job; their workflow names a fixed directory it caches.
    if(DEFINED ENV{NEXUS_CARGO_TARGET_DIR} AND NOT "$ENV{NEXUS_CARGO_TARGET_DIR}" STREQUAL "")
        set(policy_cargo_target "$ENV{NEXUS_CARGO_TARGET_DIR}")
    endif()
    set(policy_stage_archive
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${policy_target}/${policy_triple_dir}release"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${policy_cargo_target}/${policy_triple_dir}release/${CMAKE_STATIC_LIBRARY_PREFIX}nexus_policy_runtime${CMAKE_STATIC_LIBRARY_SUFFIX}"
            "${policy_archive}")
    if("$ENV{NOVA_JOB_CORES}" MATCHES "^[1-9][0-9]*$")
        list(APPEND policy_cargo_env "CARGO_BUILD_JOBS=$ENV{NOVA_JOB_CORES}")
    endif()
endif()
if(APPLE)
    list(APPEND policy_cargo_env "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
set(policy_cross_build)
if(NEXUS_RUST_TARGET)
    # Cargo runs the rustc on PATH unless told: the one beside it, whose toolchain has the target (cmake/ios.cmake).
    # Its release builds are not stripped: rustup's toolchain from Homebrew cannot run its rust-objcopy (it finds no
    # libLLVM.dylib), and the archive is stripped when the app links.
    list(APPEND policy_cargo_env "RUSTC=${NEXUS_RUSTC}" "CARGO_PROFILE_RELEASE_STRIP=false")
    set(policy_cross_build
        COMMAND "${CMAKE_COMMAND}" -E env ${policy_cargo_env} ${NEXUS_RUST_TARGET_ENV} "${NEXUS_CARGO}" build --release
            --locked --lib --target "${NEXUS_RUST_TARGET}" --manifest-path "${policy_crate}/Cargo.toml"
            --target-dir "${policy_cargo_target}")
endif()
add_custom_command(OUTPUT "${policy_archive}" "${policy_tool}"
    COMMAND "${CMAKE_COMMAND}" -E env ${policy_cargo_env} "${NEXUS_CARGO}" build --release --locked --manifest-path "${policy_crate}/Cargo.toml" --target-dir "${policy_cargo_target}"
    ${policy_cross_build}
    ${policy_stage_archive}
    COMMAND "${CMAKE_COMMAND}" -E copy "${policy_cargo_target}/release/nexus-policy${CMAKE_EXECUTABLE_SUFFIX}" "${policy_tool}"
    DEPENDS "${CMAKE_CURRENT_LIST_FILE}" "${policy_crate}/Cargo.toml" "${policy_crate}/Cargo.lock" "${policy_crate}/build.rs"
        "${policy_crate}/src/lib.rs" "${policy_crate}/src/main.rs" ${policy_sources} "${PROJECT_SOURCE_DIR}/policy/rules/config.json"
    VERBATIM)
add_custom_target(nexus_policy_tools DEPENDS "${policy_archive}" "${policy_tool}")
add_library(nexus_policy_runtime STATIC IMPORTED GLOBAL)
set_target_properties(nexus_policy_runtime PROPERTIES IMPORTED_LOCATION "${policy_archive}")
add_library(nexus_policy STATIC src/policy/host.cpp src/policy/session.cpp)
add_dependencies(nexus_policy nexus_policy_tools)
target_include_directories(nexus_policy PUBLIC src)
target_compile_options(nexus_policy PRIVATE ${NEXUS_WARNINGS})
target_link_libraries(nexus_policy PRIVATE nexus_policy_runtime prax::json)
if(APPLE)
    target_link_libraries(nexus_policy PRIVATE "-framework Security" "-framework CoreFoundation" "-liconv")
elseif(WIN32)
    target_link_libraries(nexus_policy PRIVATE bcrypt ntdll userenv ws2_32 advapi32 synchronization)
else()
    target_link_libraries(nexus_policy PRIVATE dl pthread m)
endif()
