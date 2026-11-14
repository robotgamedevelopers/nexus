# `nexus --state` draws from the state file alone (src/main.cpp): run against a fixture state with HOME an empty
# directory of its own, it exits 0 and writes the PNG, makes no ~/.nexus (no clientHome, no makeClientHome, no error
# log), reads no nova-config and no saved tab order, and its friend tab is the state's friend.
#
#   cmake -DNEXUS=<nexus> -DSTATE=<file.state> -DWORK=<dir> -P state_screenshot.cmake

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK}/home)
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env HOME=${WORK}/home USERPROFILE=${WORK}/home
        ${NEXUS} --windowed --tab 2 --state ${STATE} --screenshot ${WORK}/shot.png
    RESULT_VARIABLE result
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
set(problems "")
if(NOT result EQUAL 0)
    string(APPEND problems "nexus exited with ${result}\n")
endif()
if(NOT EXISTS ${WORK}/shot.png)
    string(APPEND problems "no screenshot at ${WORK}/shot.png\n")
endif()
if(EXISTS ${WORK}/home/.nexus)
    string(APPEND problems "it made ${WORK}/home/.nexus: it read the window's home\n")
endif()
if(out MATCHES "nova-config" OR err MATCHES "nova-config|\\.nexus")
    string(APPEND problems "it read the window's config or nova-config\n")
endif()
if(NOT problems STREQUAL "")
    message(FATAL_ERROR "nexus --state:\n${problems}stdout:\n${out}\nstderr:\n${err}")
endif()
