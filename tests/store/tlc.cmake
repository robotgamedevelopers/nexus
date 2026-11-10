# Runs one TLC check of the store's models and judges its outcome. Invoked by ctest as `cmake -D... -P tlc.cmake`;
# tests/model/tlc.cmake is the window's, and this one follows it.
#
#   WORK        a directory of its own for this run (emptied first); TLC runs on a copy of the models there
#   MODELS      models/store of the checkout (its *.tla and *.cfg, and witnesses/*.cfg, are copied)
#   MODULE      the module TLC checks (Journal, or JournalTrace)
#   CFG         the configuration file, relative to MODELS
#   EXPECT      pass | invariant | deadlock
#   PROPERTY    with invariant: the invariant whose violation is expected
#   WALK        optional: the journal_walk program, run first with WALK_ARGS (comma-separated) and
#               WORK/JournalTraceData.tla
#   WORKERS     optional: TLC's worker count, 2 unless given
#   CAP         optional: the cap on the TLC run in seconds, 60 unless given
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
    execute_process(COMMAND "${WALK}" ${walk_args} "${WORK}/JournalTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "journal_walk failed (${walk_rc}): ${walk_out}")
    endif()
endif()

if(NOT DEFINED WORKERS OR "${WORKERS}" STREQUAL "")
    set(WORKERS 2)
endif()
if(NOT DEFINED CAP OR "${CAP}" STREQUAL "")
    set(CAP 60)
endif()
if(WORKERS GREATER 2)
    set(jvm -XX:+UseParallelGC -XX:ParallelGCThreads=${WORKERS})
else()
    set(jvm -XX:+UseSerialGC -XX:TieredStopAtLevel=1)
endif()

execute_process(
    COMMAND "${java}" ${jvm} "-Djava.io.tmpdir=${WORK}/tmp" -cp "${jar}" tlc2.TLC
            -workers ${WORKERS} -metadir "${WORK}/meta" -config "${cfg_name}" "${MODULE}.tla"
    WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out TIMEOUT ${CAP})
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
else()
    message(FATAL_ERROR "tlc.cmake: EXPECT is ${EXPECT}, not pass, invariant or deadlock")
endif()
message("${MODULE} ${cfg_name}: as expected (${EXPECT} ${PROPERTY})")
