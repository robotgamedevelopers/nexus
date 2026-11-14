# Tests of the link over mosh's transport (src/link) and of nexus-server (src/server), with two labels:
#   unit        link_unit: the CONNECT line, the ssh command line, and the bootstrap over a fake command runner
#   functional  link_e2e_*: nexus-server and headless windows over real ssh to this machine and real UDP. They need
#               `ssh localhost` to work without a password prompt (BatchMode), as it does on the studio's runners.
#               NEXUS_E2E_TAILNET names a second way to reach this same machine (its tailnet address or name); when
#               set, the round trip runs through it too. server_home_functional: nexus-server's own home under a
#               HOME of the test's own, with no --dir.

set(warnings ${NEXUS_WARNINGS})
set(NEXUS_E2E_TAILNET "" CACHE STRING "This machine's tailnet address or name, for the functional round trip over it")

add_executable(link_unit link_unit.cpp)
target_link_libraries(link_unit PRIVATE nexus_link nexus_server)
target_compile_options(link_unit PRIVATE ${warnings})

add_test(NAME link_unit COMMAND link_unit)
set_tests_properties(link_unit PROPERTIES LABELS unit TIMEOUT 30)

add_executable(link_e2e link_e2e.cpp)
target_link_libraries(link_e2e PRIVATE nexus_link)
target_include_directories(link_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sync)
target_compile_options(link_e2e PRIVATE ${warnings})
add_dependencies(link_e2e nexus-server nexus_fake_driver)

# Each test's server lives in a directory of its own under /tmp (a Unix socket's path must be short), named by a hash
# of this build tree, so that two builds on one machine (two CI runners) never share one. The cleanup fixture stops
# every one of them after the tests, whatever became of them; each test also stops what an earlier run left first.
string(MD5 build_hash "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${build_hash}" 0 10 build_hash)
set(e2e_dir "/tmp/nexus-e2e-${build_hash}")
set(server $<TARGET_FILE:nexus-server>)

add_test(NAME link_e2e_loopback COMMAND link_e2e ${server} ${e2e_dir}-loopback roundtrip localhost $<TARGET_FILE:nexus_fake_driver>)
add_test(NAME link_e2e_outage COMMAND link_e2e ${server} ${e2e_dir}-outage outage)
add_test(NAME link_e2e_soak COMMAND link_e2e ${server} ${e2e_dir}-soak soak 6 300)
add_test(NAME link_e2e_restart COMMAND link_e2e ${server} ${e2e_dir}-restart restart)
add_test(NAME link_e2e_older_server COMMAND link_e2e ${server} ${e2e_dir}-older older_server)
set(e2e_tests link_e2e_loopback link_e2e_outage link_e2e_soak link_e2e_restart link_e2e_older_server)
set(e2e_dirs ${e2e_dir}-loopback ${e2e_dir}-outage ${e2e_dir}-soak ${e2e_dir}-restart ${e2e_dir}-older)
if(NEXUS_E2E_TAILNET)
    add_test(NAME link_e2e_tailnet COMMAND link_e2e ${server} ${e2e_dir}-tailnet roundtrip ${NEXUS_E2E_TAILNET}
        $<TARGET_FILE:nexus_fake_driver>)
    list(APPEND e2e_tests link_e2e_tailnet)
    list(APPEND e2e_dirs ${e2e_dir}-tailnet)
endif()
# The runs fail a link that stops making progress themselves, 30 s after its last step (kStallSeconds in link_e2e.cpp),
# whatever the clock says, so a slow run that is still progressing is not cut: the server's flush per pass waits on
# every other writer to the drive. TIMEOUT is only the backstop for a process blocked outside any wait (ssh, a socket
# call).
set_tests_properties(${e2e_tests} PROPERTIES LABELS functional TIMEOUT 600 FIXTURES_REQUIRED link_e2e_servers)
add_test(NAME link_e2e_stop_servers COMMAND link_e2e ${server} stop ${e2e_dirs})
set_tests_properties(link_e2e_stop_servers PROPERTIES LABELS functional TIMEOUT 60
    FIXTURES_CLEANUP link_e2e_servers)

# nexus-server's own home with no --dir (src/server/home.h): HOME is a directory of the test's own under /tmp, named for
# this build tree like the servers above.
add_executable(server_home server_home.cpp)
target_compile_options(server_home PRIVATE ${warnings})
add_dependencies(server_home nexus-server)
add_test(NAME server_home_functional COMMAND server_home ${server} /tmp/nexus-home-${build_hash})
set_tests_properties(server_home_functional PROPERTIES LABELS functional TIMEOUT 60)
