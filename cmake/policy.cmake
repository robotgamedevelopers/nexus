# Both production hosts embed the same Starlark interpreter. The CLI uses it to preview and apply a fork's rules.
find_program(NEXUS_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
set(policy_crate "${PROJECT_SOURCE_DIR}/policy/native")
set(policy_target "${CMAKE_BINARY_DIR}/policy-rust")
set(policy_archive "${policy_target}/release/${CMAKE_STATIC_LIBRARY_PREFIX}nexus_policy_runtime${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(policy_tool "${CMAKE_BINARY_DIR}/nexus-policy${CMAKE_EXECUTABLE_SUFFIX}")
file(GLOB policy_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/policy/rules/*.star")
set(policy_cargo_env)
if(APPLE)
    list(APPEND policy_cargo_env "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
add_custom_command(OUTPUT "${policy_archive}" "${policy_tool}"
    COMMAND "${CMAKE_COMMAND}" -E env ${policy_cargo_env} "${NEXUS_CARGO}" build --release --locked --manifest-path "${policy_crate}/Cargo.toml" --target-dir "${policy_target}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${policy_target}/release/nexus-policy${CMAKE_EXECUTABLE_SUFFIX}" "${policy_tool}"
    DEPENDS "${CMAKE_CURRENT_LIST_FILE}" "${policy_crate}/Cargo.toml" "${policy_crate}/Cargo.lock" "${policy_crate}/build.rs"
        "${policy_crate}/src/lib.rs" "${policy_crate}/src/main.rs" ${policy_sources} "${PROJECT_SOURCE_DIR}/policy/rules/config.json"
    VERBATIM)
add_custom_target(nexus_policy_tools DEPENDS "${policy_archive}" "${policy_tool}")
add_library(nexus_policy_runtime STATIC IMPORTED GLOBAL)
set_target_properties(nexus_policy_runtime PROPERTIES IMPORTED_LOCATION "${policy_archive}")
add_library(nexus_policy STATIC src/policy/host.cpp)
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
