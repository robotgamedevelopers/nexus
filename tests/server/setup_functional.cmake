# `nexus-server setup`, its handover and `doctor` on the real filesystem, in a HOME of the test's own (a short one under
# /tmp, so the control socket's path fits). Functional: real processes, a real socket, real time.
#
#   cmake -DSERVER=<nexus-server> -DSOURCE=<the fork> [-DMODE=service] -P setup_functional.cmake
#
# Without MODE (no service registered, --no-service): the first run places the server and the handover starts it; a
# second run changes nothing; a build with no tested mark is refused and nothing changes; a tested build of other bytes
# takes over from the running server (the `build` and `takeover` requests, the drain and the exit 75); a tested build
# that crashes as it opens the store (a script that passes the probe on an empty home and exits at once on this one) is
# placed, does not come up, and the handover puts the previous build back, which runs again; a build placed with
# --wait 0 is handed over by the handover process after setup has exited, with no service to start it; doctor names
# the rollback and what is left with its command; then `stop`.
#
# MODE=service: the same crash with the per-user service registered (a launchd agent, or a systemd user unit), named for
# this HOME and removed at the end (lingering, if setup turned it on, turned off again). Skipped, saying so, where this
# machine registers none (no user manager for this user).
#
# Each program placed is a copy in a directory of its own, with its <program>.tested written from `build-of`, as
# rebuild writes it once the tests pass.

if(NOT SERVER OR NOT SOURCE)
    message(FATAL_ERROR "usage: cmake -DSERVER=<nexus-server> -DSOURCE=<the fork> [-DMODE=service] "
        "-P setup_functional.cmake")
endif()
execute_process(COMMAND mktemp -d /tmp/nxsetup.XXXXXX OUTPUT_VARIABLE home OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE made)
if(NOT made EQUAL 0)
    message(FATAL_ERROR "cannot make a HOME under /tmp")
endif()
set(ENV{HOME} "${home}")
set(failed "")
set(dir "${home}/.nexus/server")
set(placed "${home}/.nexus/bin/nexus-server")
if(MODE STREQUAL "service")
    set(service_flag "")
else()
    set(service_flag "--no-service")
endif()

function(build_of file)
    execute_process(COMMAND ${SERVER} build-of "${file}" OUTPUT_VARIABLE b OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(build "${b}" PARENT_SCOPE)
endfunction()

# A program `name`: SERVER's bytes and `extra`, in ${home}/name, tested unless `tested` is OFF.
function(make_program name extra tested)
    set(p "${home}/${name}/nexus-server")
    file(MAKE_DIRECTORY "${home}/${name}")
    file(COPY_FILE "${SERVER}" "${p}")
    if(NOT extra STREQUAL "")
        file(APPEND "${p}" "${extra}")
    endif()
    file(CHMOD "${p}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    if(tested)
        build_of("${p}")
        file(WRITE "${p}.tested" "${build}\n")
    endif()
endfunction()

# A program that crashes as it opens this HOME's store: as `serve` with HOME this test's, it exits at once; anywhere
# else (setup's probe, on an empty home of its own) it is `real`. Tested.
function(make_crasher name real)
    set(p "${home}/${name}/nexus-server")
    file(MAKE_DIRECTORY "${home}/${name}")
    file(WRITE "${p}" "#!/bin/sh\n# A build that crashes at start on the store (tests/server/setup_functional.cmake).\n"
        "if [ \"$1\" = serve ] && [ \"$HOME\" = \"${home}\" ]; then\n"
        "    echo \"crash-at-start test build: exiting before the store opens\" >&2\n"
        "    exit 3\n"
        "fi\n"
        "exec \"${real}\" \"$@\"\n")
    file(CHMOD "${p}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    build_of("${p}")
    file(WRITE "${p}.tested" "${build}\n")
endfunction()

function(run_setup program label)
    execute_process(COMMAND ${program} setup ${service_flag} --start-wait 3 ${ARGN}
                    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    message(STATUS "${label} (exit ${rc}):\n${out}${err}")
    set(out "${out}" PARENT_SCOPE)
    set(rc "${rc}" PARENT_SCOPE)
endfunction()

function(expect text what)
    string(FIND "${out}" "${what}" at)
    if(at EQUAL -1)
        set(failed "${failed}\n  ${text}: no \"${what}\"" PARENT_SCOPE)
    endif()
endfunction()

function(refuse text what)
    string(FIND "${out}" "${what}" at)
    if(NOT at EQUAL -1)
        set(failed "${failed}\n  ${text}: \"${what}\"" PARENT_SCOPE)
    endif()
endfunction()

# A setup that goes well exits 0, but only where the fork's Claude driver is built (npm ci && npm run build in
# drivers/claude): without it, setup's driver steps FAIL and it exits 1, and the other lines are still checked.
if(EXISTS "${SOURCE}/drivers/claude/dist/src/main.js" AND IS_DIRECTORY "${SOURCE}/drivers/claude/node_modules")
    set(good 0)
    set(background 3)
else()
    set(good 1)
    set(background 1)
    message(STATUS "setup_functional: the Claude driver is not built in ${SOURCE}, so setup exits 1 where it would "
        "exit 0 or 3")
endif()

function(expect_exit text want)
    if(want STREQUAL "good")
        set(want "${good}")
    endif()
    if(NOT rc STREQUAL "${want}")
        set(failed "${failed}\n  ${text}: exit ${rc}, not ${want}" PARENT_SCOPE)
    endif()
endfunction()

# The placed program is `program`'s bytes.
function(expect_placed text program)
    build_of("${placed}")
    set(have "${build}")
    build_of("${program}")
    if(NOT have STREQUAL build)
        set(failed "${failed}\n  ${text}: the placed program is build ${have}, not ${program}'s ${build}" PARENT_SCOPE)
    endif()
endfunction()

# The running server answers `build` with `program`'s build (doctor's line says so).
function(expect_runs text program)
    execute_process(COMMAND ${program} doctor OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(out "${out}" PARENT_SCOPE)
    string(FIND "${out}" "ok    the server runs the placed build" at)
    if(at EQUAL -1)
        set(failed "${failed}\n  ${text}: doctor does not say the placed build runs:\n${out}" PARENT_SCOPE)
    endif()
endfunction()

# Removes the service this HOME registered, if any.
function(remove_service)
    file(GLOB plists "${home}/Library/LaunchAgents/*.plist")
    foreach(plist ${plists})
        execute_process(COMMAND id -u OUTPUT_VARIABLE uid OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND launchctl bootout gui/${uid} "${plist}" OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND launchctl bootout user/${uid} "${plist}" OUTPUT_QUIET ERROR_QUIET)
    endforeach()
    file(GLOB units "${home}/.config/systemd/user/*.service")
    foreach(unit ${units})
        get_filename_component(name "${unit}" NAME)
        execute_process(COMMAND systemctl --user disable --now "${name}" OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND systemctl --user reset-failed "${name}" OUTPUT_QUIET ERROR_QUIET)
    endforeach()
endfunction()

make_program(a "" ON)

# 1. First setup: placed, started by the handover.
run_setup("${home}/a/nexus-server" "first setup")
expect("first" "done  the server is placed: ${placed}")
expect("first" "start the handover")
expect("first" "ok    the server runs the placed build")
expect("first" "the accounts and friends live in ${dir}/nexus-config")
expect_placed("first" "${home}/a/nexus-server")
if(NOT IS_SYMLINK "${home}/.local/bin/nexus-server")
    set(failed "${failed}\n  first: ~/.local/bin/nexus-server is not a link")
endif()

if(MODE STREQUAL "service")
    string(FIND "${out}" "is loaded" loaded)
    string(FIND "${out}" "is enabled" enabled)
    # Lingering that setup turned on for this user is turned off again at the end: the test leaves it as it found it.
    string(FIND "${out}" "; lingering is on" lingerTurnedOn)
    if(loaded EQUAL -1 AND enabled EQUAL -1)
        remove_service()
        execute_process(COMMAND "${home}/a/nexus-server" stop OUTPUT_QUIET ERROR_QUIET)
        file(REMOVE_RECURSE "${home}")
        message(STATUS "setup_functional (service): skipped: this machine registers no per-user service")
        return()
    endif()
    # The service's file, with its PATH computed, never setup's own: the same whatever PATH setup runs under.
    file(GLOB serviceFiles "${home}/Library/LaunchAgents/*.plist" "${home}/.config/systemd/user/*.service")
    file(READ "${serviceFiles}" serviceBefore)
    # 2s. A build that crashes at start on the store, with the service restarting it: rolled back.
    make_crasher(c "${home}/a/nexus-server")
    set(pathBefore "$ENV{PATH}")
    set(ENV{PATH} "${home}/nowhere:$ENV{PATH}")
    run_setup("${home}/a/nexus-server" "setup of a build that crashes at start, with a service" --program
        "${home}/c/nexus-server")
    set(ENV{PATH} "${pathBefore}")
    file(READ "${serviceFiles}" serviceAfter)
    if(NOT serviceBefore STREQUAL serviceAfter)
        set(failed "${failed}\n  service: setup rewrote ${serviceFiles} under another PATH")
    endif()
    string(FIND "${serviceAfter}" "/usr/bin:/bin" systemDirs)
    if(systemDirs EQUAL -1)
        set(failed "${failed}\n  service: ${serviceFiles} has no PATH of the system's directories")
    endif()
    expect_exit("service crash" 1)
    expect("service crash" "did not come up on the store within 3 s, so the handover put the previous build")
    expect_placed("service crash" "${home}/a/nexus-server")
    expect_runs("service crash" "${home}/a/nexus-server")
    expect("service crash doctor" "did not come up on the store, so the previous build")
    execute_process(COMMAND "${home}/a/nexus-server" stop OUTPUT_QUIET ERROR_QUIET)
    remove_service()
    if(NOT lingerTurnedOn EQUAL -1)
        execute_process(COMMAND loginctl disable-linger OUTPUT_QUIET ERROR_QUIET)
    endif()
else()
    # 2. Again: nothing to do.
    run_setup("${home}/a/nexus-server" "second setup")
    expect_exit("second" good)
    expect("second" "ok    the server is placed")
    expect("second" "ok    the server runs the placed build")
    refuse("second" "start ")
    refuse("second" "drain ")

    # 3. A build with no tested mark: refused, and nothing changes.
    make_program(b "x" OFF)
    run_setup("${home}/b/nexus-server" "setup of an untested build")
    expect_exit("untested" 1)
    expect("untested" "has not passed its tests")
    expect("untested" "Nothing was changed")
    expect_placed("untested" "${home}/a/nexus-server")

    # 4. Tested, it takes over.
    build_of("${home}/b/nexus-server")
    file(WRITE "${home}/b/nexus-server.tested" "${build}\n")
    run_setup("${home}/b/nexus-server" "setup from another build")
    expect_exit("takeover" good)
    expect("takeover" "done  the server is placed: ${placed}")
    expect("takeover" "drain the server runs build")
    expect("takeover" "ok    the server runs the placed build")
    expect_placed("takeover" "${home}/b/nexus-server")
    file(READ "${dir}/server.log" log)
    string(FIND "${log}" "drained for a takeover" drained)
    if(drained EQUAL -1)
        set(failed "${failed}\n  takeover: the log has no \"drained for a takeover\"")
    endif()
    build_of("${placed}.previous")
    set(kept "${build}")
    build_of("${home}/a/nexus-server")
    if(NOT kept STREQUAL build)
        set(failed "${failed}\n  takeover: ${placed}.previous is not the build placed before")
    endif()

    # 5. A build that crashes at start on the store, with no service: the handover starts it, it never answers, and
    # the handover puts the previous build back and starts that.
    make_crasher(c "${home}/b/nexus-server")
    run_setup("${home}/b/nexus-server" "setup of a build that crashes at start" --program "${home}/c/nexus-server")
    expect_exit("crash" 1)
    expect("crash" "ok    the build")
    expect("crash" "did not come up on the store within 3 s, so the handover put the previous build")
    expect_placed("crash" "${home}/b/nexus-server")
    expect_runs("crash" "${home}/b/nexus-server")
    expect("crash doctor" "did not come up on the store, so the previous build")
    file(READ "${dir}/handover.log" handoverLog)
    string(FIND "${handoverLog}" "roll-back" rolled)
    if(rolled EQUAL -1)
        set(failed "${failed}\n  crash: the handover log has no roll-back:\n${handoverLog}")
    endif()

    # 6. With no service, setup gone at once (--wait 0): the handover process finishes the takeover by itself.
    make_program(d "xx" ON)
    run_setup("${home}/d/nexus-server" "setup that does not wait" --wait 0)
    expect_exit("background" "${background}")
    expect("background" "the handover still runs, in the background")
    set(state "")
    foreach(i RANGE 100)
        if(EXISTS "${dir}/handover")
            file(READ "${dir}/handover" state)
        endif()
        if(state MATCHES "^done ")
            break()
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.2)
    endforeach()
    if(NOT state MATCHES "^done ")
        set(failed "${failed}\n  background: the handover did not end done: \"${state}\"")
    endif()
    expect_placed("background" "${home}/d/nexus-server")
    expect_runs("background" "${home}/d/nexus-server")

    # 7. Doctor: the missing service is named with its command.
    execute_process(COMMAND "${home}/d/nexus-server" doctor OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message(STATUS "doctor:\n${out}${err}")
    expect("doctor" "fix   no service keeps the server running across logout and reboot: nexus-server setup")
    execute_process(COMMAND "${home}/d/nexus-server" stop OUTPUT_QUIET ERROR_QUIET)
endif()

# Clean up.
file(REMOVE_RECURSE "${home}")
if(failed)
    message(FATAL_ERROR "setup_functional:${failed}")
endif()
message(STATUS "setup_functional: ok")
