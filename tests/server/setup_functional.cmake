# `nexus-server setup` and `doctor` on the real filesystem, in a HOME of the test's own (a short one under /tmp, so the
# control socket's path fits), with no service registered (--no-service): the first run places the server and starts
# it; a second run changes nothing; a build of other bytes placed by its own setup takes over from the running server
# (the `build` and `takeover` requests, the drain and the exit 75); doctor names what is left with its command; then
# `stop`. Functional: real processes, a real socket, real time.
#
#   cmake -DSERVER=<nexus-server> -P setup_functional.cmake

if(NOT SERVER)
    message(FATAL_ERROR "usage: cmake -DSERVER=<nexus-server> -P setup_functional.cmake")
endif()
execute_process(COMMAND mktemp -d /tmp/nxsetup.XXXXXX OUTPUT_VARIABLE home OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE made)
if(NOT made EQUAL 0)
    message(FATAL_ERROR "cannot make a HOME under /tmp")
endif()
set(ENV{HOME} "${home}")
set(failed "")

function(run_setup program label)
    execute_process(COMMAND ${program} setup --no-service --wait 20 OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message(STATUS "${label}:\n${out}${err}")
    set(out "${out}" PARENT_SCOPE)
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

# 1. First setup: placed, started.
run_setup(${SERVER} "first setup")
expect("first" "done  the server is placed: ${home}/.nexus/bin/nexus-server")
expect("first" "start the server starts")
expect("first" "ok    the server runs the placed build")
expect("first" "the accounts and friends live in ${home}/.nexus/server/nexus-config")
if(NOT IS_SYMLINK "${home}/.local/bin/nexus-server")
    set(failed "${failed}\n  first: ~/.local/bin/nexus-server is not a link")
endif()

# 2. Again: nothing to do.
run_setup(${SERVER} "second setup")
expect("second" "ok    the server is placed")
expect("second" "ok    the server runs the placed build")
refuse("second" "start ")
refuse("second" "drain ")

# 3. Another build (the same program with a byte more, so another build id) takes over.
set(other "${home}/other-nexus-server")
file(COPY_FILE "${SERVER}" "${other}")
file(APPEND "${other}" "x")
file(CHMOD "${other}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
run_setup(${other} "setup from another build")
expect("takeover" "done  the server is placed: ${home}/.nexus/bin/nexus-server")
expect("takeover" "drain the server runs build")
expect("takeover" "ok    the server runs the placed build")
file(READ "${home}/.nexus/server/server.log" log)
string(FIND "${log}" "drained for a takeover" drained)
if(drained EQUAL -1)
    set(failed "${failed}\n  takeover: the log has no \"drained for a takeover\"")
endif()

# 4. Doctor: the running server is the placed build; the missing service is named with its command.
execute_process(COMMAND ${other} doctor OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "doctor:\n${out}${err}")
expect("doctor" "ok    the server runs the placed build")
expect("doctor" "fix   no service keeps the server running across logout and reboot: nexus-server setup")

# 5. Stop, and clean up.
execute_process(COMMAND ${other} stop OUTPUT_QUIET ERROR_QUIET)
file(REMOVE_RECURSE "${home}")
if(failed)
    message(FATAL_ERROR "setup_functional:${failed}")
endif()
message(STATUS "setup_functional: ok")
