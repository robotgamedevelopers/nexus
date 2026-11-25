# Both production hosts embed the same Starlark interpreter. The CLI uses it to preview and apply a fork's rules.
find_program(NEXUS_CARGO cargo HINTS "$ENV{HOME}/sdk/bin" "$ENV{USERPROFILE}/sdk/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
set(policy_crate "${PROJECT_SOURCE_DIR}/policy/native")
set(policy_target "${CMAKE_BINARY_DIR}/policy-rust")
set(policy_archive "${policy_target}/release/${CMAKE_STATIC_LIBRARY_PREFIX}nexus_policy_runtime${CMAKE_STATIC_LIBRARY_SUFFIX}")
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
    set(policy_cache_home "$ENV{HOME}")
    if(WIN32)
        set(policy_cache_home "$ENV{USERPROFILE}")
    endif()
    set(policy_cargo_target "${policy_cache_home}/.cache/nexus-ci/$ENV{RUNNER_NAME}/starlark")
    set(policy_stage_archive
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${policy_target}/release"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${policy_cargo_target}/release/${CMAKE_STATIC_LIBRARY_PREFIX}nexus_policy_runtime${CMAKE_STATIC_LIBRARY_SUFFIX}"
            "${policy_archive}")
    if("$ENV{NOVA_JOB_CORES}" MATCHES "^[1-9][0-9]*$")
        list(APPEND policy_cargo_env "CARGO_BUILD_JOBS=$ENV{NOVA_JOB_CORES}")
    endif()
endif()
if(APPLE)
    list(APPEND policy_cargo_env "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
add_custom_command(OUTPUT "${policy_archive}" "${policy_tool}"
    COMMAND "${CMAKE_COMMAND}" -E env ${policy_cargo_env} "${NEXUS_CARGO}" build --release --locked --manifest-path "${policy_crate}/Cargo.toml" --target-dir "${policy_cargo_target}"
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
