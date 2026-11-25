# The mock (docs/mock.md): nexus-mock, a real nexus-server with fake AIs on the fake driver and seeded chats, for
# driving the clients. Built with the link (it is the link's server), not on Windows (the fake driver is POSIX). Labels:
#   unit        mock_unit: what the mock writes (names, presets, scripts, drivers, accounts, config, the client
#               command); pure, no files, time or processes
#   functional  mock_e2e: nexus-mock started for real, a client connected over the real link on loopback: the AIs and
#               their seeded transcripts arrive, a prompt gets its scripted reply exactly once, the sign-in preset's AI
#               alone needs a sign-in, and the state with no AIs

set(warnings ${NEXUS_WARNINGS})

add_library(nexus_mock_logic STATIC mock.cpp)
target_include_directories(nexus_mock_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nexus_mock_logic PUBLIC nexus_accounts)
target_compile_options(nexus_mock_logic PRIVATE ${warnings})

add_library(nexus_mock_link STATIC mock_link.cpp)
target_link_libraries(nexus_mock_link PUBLIC nexus_mock_logic nexus_link)
target_compile_options(nexus_mock_link PRIVATE ${warnings})

add_executable(nexus_mock nexus_mock.cpp)
target_link_libraries(nexus_mock PRIVATE nexus_mock_link)
target_compile_options(nexus_mock PRIVATE ${warnings})
set_target_properties(nexus_mock PROPERTIES OUTPUT_NAME nexus-mock)
target_compile_definitions(nexus_mock PRIVATE
    NEXUS_MOCK_SERVER="$<TARGET_FILE:nexus-server>"
    NEXUS_MOCK_FAKE_DRIVER="$<TARGET_FILE:nexus_fake_driver>"
    NEXUS_MOCK_FAKE_CLAUDE="$<TARGET_FILE:nexus_fake_claude>")
add_dependencies(nexus_mock nexus-server nexus_fake_driver nexus_fake_claude)

add_executable(mock_unit mock_unit.cpp)
target_link_libraries(mock_unit PRIVATE nexus_mock_logic nexus_driver nexus_supervisor)
target_compile_options(mock_unit PRIVATE ${warnings})
add_test(NAME mock_unit COMMAND mock_unit)
set_tests_properties(mock_unit PROPERTIES LABELS unit TIMEOUT 30)

# The mock's directory lives under /tmp (a Unix socket's path must be short), named by a hash of this build tree so that
# two builds on one machine never share one; the test removes it at its end, and a run removes what an earlier one left.
add_executable(mock_e2e mock_e2e.cpp)
target_link_libraries(mock_e2e PRIVATE nexus_mock_link)
target_compile_options(mock_e2e PRIVATE ${warnings})
add_dependencies(mock_e2e nexus_mock)
string(MD5 mock_hash "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${mock_hash}" 0 10 mock_hash)
add_test(NAME mock_e2e COMMAND mock_e2e $<TARGET_FILE:nexus_mock> /tmp/nexus-mock-e2e-${mock_hash})
set_tests_properties(mock_e2e PROPERTIES LABELS functional TIMEOUT 300)
