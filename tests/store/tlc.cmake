# Runs one TLC check of the store's models and judges its outcome. Invoked by ctest as `cmake -D... -P tlc.cmake`;
# tests/model/tlc.cmake is the window's, and this one follows it.
#
#   WORK        a directory of its own for this run (emptied first); TLC runs on a copy of the models there
#   MODELS      models/store of the checkout (its *.tla and *.cfg, and witnesses/*.cfg, are copied)
#   MODULE      the module TLC checks (Journal, JournalTrace, StoreLock, StoreLockTrace)
#   CFG         the configuration file, relative to MODELS
#   EXPECT      pass | invariant | deadlock | temporal | action
#   PROPERTY    with invariant: the invariant whose violation is expected; with temporal: the configuration's one
#               PROPERTY, whose violation is expected; with action: the action property ([][A]_v) expected violated
#   WALK        optional: the walk program (journal_walk, store_lock_walk), run first with WALK_ARGS (comma-separated)
#               and WORK/WALK_OUT
#   WALK_OUT    optional: the trace module the walk writes, JournalTraceData.tla unless given
#   WORKERS     optional: TLC's worker count, 2 unless given
#   CAP         optional: the cap on the TLC run in seconds, 60 unless given; 0 for none (NEXUS_TLC_UNCAPPED)
#
# The TLC jar is $ENV{TLA2TOOLS_JAR}; java is $ENV{JAVA}, else `java` on PATH.

cmake_minimum_required(VERSION 3.28)

foreach(v WORK MODELS MODULE CFG EXPECT)
    if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: ${v} is not set")
    endif()
endforeach()

if("$ENV{TLA2TOOLS_JAR}" STREQUAL "")
    message(FATAL_ERROR
        "TLA2TOOLS_JAR is not set. Set it to the path of tla2tools.jar (the TLA+ tools, github.com/tlaplus/tlaplus "
        "releases), for example: export TLA2TOOLS_JAR=/path/to/tla2tools.jar. The model tests cannot run without it.")
endif()
set(jar "$ENV{TLA2TOOLS_JAR}")
if(NOT EXISTS "${jar}")
    message(FATAL_ERROR "TLA2TOOLS_JAR is ${jar}, which does not exist. Set it to the path of tla2tools.jar.")
endif()
if(NOT "$ENV{JAVA}" STREQUAL "")
    set(java "$ENV{JAVA}")
else()
    find_program(java java)
    if(NOT java)
        message(FATAL_ERROR "No java on PATH. Put a Java 11 or newer on PATH, or set JAVA to its path.")
    endif()
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/tmp")
file(GLOB specs "${MODELS}/*.tla" "${MODELS}/*.cfg" "${MODELS}/witnesses/*.cfg")
file(COPY ${specs} DESTINATION "${WORK}")
get_filename_component(cfg_name "${CFG}" NAME)

if(DEFINED WALK AND NOT "${WALK}" STREQUAL "")
    string(REPLACE "," ";" walk_args "${WALK_ARGS}")
    if(NOT DEFINED WALK_OUT OR "${WALK_OUT}" STREQUAL "")
        set(WALK_OUT JournalTraceData.tla)
    endif()
    execute_process(COMMAND "${WALK}" ${walk_args} "${WORK}/${WALK_OUT}"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "${WALK} failed (${walk_rc}): ${walk_out}")
    endif()
endif()

if(NOT DEFINED CAP OR "${CAP}" STREQUAL "")
    set(CAP 60)
endif()
# CAP 0 (NEXUS_TLC_UNCAPPED, the nightly): no cap, and TLC runs until it finishes.
set(cap_arg)
if(CAP GREATER 0)
    set(cap_arg TIMEOUT ${CAP})
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../model/tlc-workers.cmake")

execute_process(
    COMMAND "${java}" ${tlc_jvm} "-Djava.io.tmpdir=${WORK}/tmp" -cp "${jar}" tlc2.TLC
            -workers ${tlc_workers} -metadir "${WORK}/meta" -config "${cfg_name}" "${MODULE}.tla"
    WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out ${cap_arg})
file(WRITE "${WORK}/tlc.log" "${out}")

string(REGEX MATCH "generated, [0-9,]+ distinct states found" states "${out}")
string(REGEX MATCH "Finished in [^\n]*" took "${out}")
message("${MODULE} ${cfg_name}: exit ${rc}; ${states}; ${took}; log ${WORK}/tlc.log")

if(EXPECT STREQUAL "pass")
    if(NOT rc EQUAL 0 OR NOT out MATCHES "Model checking completed. No error has been found.")
        message(FATAL_ERROR "expected no error, TLC exit ${rc}:\n${out}")
    endif()
elseif(EXPECT STREQUAL "invariant")
    if(NOT out MATCHES "Invariant ${PROPERTY} is violated")
        message(FATAL_ERROR "expected invariant ${PROPERTY} to be violated, TLC exit ${rc}:\n${out}")
    endif()
elseif(EXPECT STREQUAL "deadlock")
    if(NOT out MATCHES "Deadlock reached")
        message(FATAL_ERROR "expected a deadlock, TLC exit ${rc}:\n${out}")
    endif()
elseif(EXPECT STREQUAL "temporal")
    # The configuration checks one PROPERTY, so the violation TLC reports is that one.
    if(NOT out MATCHES "Temporal properties were violated")
        message(FATAL_ERROR "expected the temporal property ${PROPERTY} to be violated, TLC exit ${rc}:\n${out}")
    endif()
elseif(EXPECT STREQUAL "action")
    if(NOT out MATCHES "Action property ${PROPERTY} is violated")
        message(FATAL_ERROR "expected the action property ${PROPERTY} to be violated, TLC exit ${rc}:\n${out}")
    endif()
else()
    message(FATAL_ERROR "tlc.cmake: EXPECT is ${EXPECT}, not pass, invariant, deadlock, temporal or action")
endif()
message("${MODULE} ${cfg_name}: as expected (${EXPECT} ${PROPERTY})")
