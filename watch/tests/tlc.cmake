# One TLC run of a watch model, for ctest (watch/tests/CMakeLists.txt), in script mode:
#   cmake -DJAVA=<java> -DJAR=<tla2tools.jar> -DMODELS=<watch/models> -DWORK=<dir> -DMODULE=<module> -DCFG=<cfg>
#         -DEXPECT=<pass | fail:Property> [-DWALK=<program> -DSEED=<s> -DSTEPS=<n> -DDATA=<module> [-DFAULT=<f>]]
#         -P tlc.cmake
# The work directory is made afresh with the models, the witnesses and the instance. With WALK, the walk runs first and
# writes the trace module DATA into it (a trace check). EXPECT pass needs TLC's "No error has been found"; fail:P needs
# TLC to report P violated (an invariant or action property by name; a temporal one as "Temporal properties were
# violated"), or, for a trace check, any step the spec cannot take (a deadlock) or a mismatch: a reversed witness or a
# planted fault that TLC does not reject fails the test. Nothing is skipped: no java or jar fails the test, saying how
# to get them.

foreach(v JAVA JAR MODELS WORK MODULE CFG EXPECT)
    if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: ${v} is not set")
    endif()
endforeach()
if(NOT EXISTS "${JAVA}")
    message(FATAL_ERROR "tlc.cmake: no java (${JAVA}). Install a JDK 21 and configure with it on the PATH")
endif()
if(NOT EXISTS "${JAR}")
    message(FATAL_ERROR "tlc.cmake: no TLC jar (${JAR}). Set TLA2TOOLS_JAR to the pinned tla2tools.jar v1.7.4 "
        "(sha256 in .github/tla2tools.sha256) and configure again")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(GLOB specs "${MODELS}/*.tla" "${MODELS}/*.cfg" "${MODELS}/witnesses/*.tla" "${MODELS}/witnesses/*.cfg")
file(COPY ${specs} DESTINATION "${WORK}")

if(DEFINED WALK AND NOT "${WALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}" "${WORK}/${DATA}.tla")
    if(DEFINED FAULT AND NOT "${FAULT}" STREQUAL "")
        list(APPEND walk_args "${FAULT}")
    endif()
    execute_process(COMMAND "${WALK}" ${walk_args} RESULT_VARIABLE walked OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out)
    if(NOT walked EQUAL 0 OR NOT EXISTS "${WORK}/${DATA}.tla")
        message(FATAL_ERROR "tlc.cmake: ${WALK} ${walk_args} failed (${walked}) and wrote no ${DATA}.tla:\n${walk_out}")
    endif()
endif()

execute_process(COMMAND "${JAVA}" -XX:+UseParallelGC -cp "${JAR}" tlc2.TLC -workers 2 -metadir "${WORK}/states"
                        -config "${CFG}" "${MODULE}"
                WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE tlc_result OUTPUT_VARIABLE out ERROR_VARIABLE out)

if(EXPECT STREQUAL "pass")
    if(NOT tlc_result EQUAL 0 OR NOT out MATCHES "Model checking completed. No error has been found")
        message(FATAL_ERROR "TLC on ${MODULE} with ${CFG} did not pass (${tlc_result}):\n${out}")
    endif()
    string(REGEX MATCH "([0-9,]+) distinct states found" found "${out}")
    message(STATUS "TLC ${MODULE} ${CFG}: passed, ${found}")
    return()
endif()

if(NOT EXPECT MATCHES "^fail:(.+)$")
    message(FATAL_ERROR "tlc.cmake: EXPECT is pass or fail:<Property>, not ${EXPECT}")
endif()
set(property "${CMAKE_MATCH_1}")
# What a semantic error, a parse error or a crash says: never a rejection.
if(out MATCHES "Parsing or semantic analysis failed|unexpected exception|Unknown operator|was not defined")
    message(FATAL_ERROR "TLC on ${MODULE} with ${CFG} failed for the wrong reason (the spec itself):\n${out}")
endif()
if(property STREQUAL "Trace")
    if(out MATCHES "Deadlock reached|is violated")
        message(STATUS "TLC ${MODULE} ${CFG}: the trace was rejected, as it must be")
        return()
    endif()
elseif(out MATCHES "${property} is violated" OR
       (out MATCHES "Temporal properties were violated" AND NOT out MATCHES "Invariant [A-Za-z]+ is violated"))
    message(STATUS "TLC ${MODULE} ${CFG}: ${property} rejected it, as it must")
    return()
endif()
message(FATAL_ERROR "TLC on ${MODULE} with ${CFG} did not reject it by ${property} (${tlc_result}):\n${out}")
