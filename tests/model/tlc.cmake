# Runs one TLC check and judges its outcome. Invoked by ctest as `cmake -D... -P tlc.cmake`.
#
#   WORK        a directory of its own for this run (emptied first); TLC runs on a copy of the models there,
#               so the checkout never gains the files TLC writes beside a spec
#   MODELS      the models/ directory of the checkout (its *.tla and witnesses/*.tla are copied)
#   MODULE      the module TLC checks (e.g. MCWindow)
#   CFG         the configuration file, relative to MODELS (e.g. MCWindow.cfg or witnesses/WSendToActive.cfg)
#   EXPECT      pass | invariant | action | liveness | deadlock
#   PROPERTY    with invariant or action: the property whose violation is expected (TLC does not name the temporal
#               property it found violated, so with liveness the cfg names the one temporal property it checks)
#   WALK        optional: the walk program (trace_walk, or label_walk); it is run first and writes DATA.tla into WORK
#   DATA        with WALK: the module the walk writes, WindowTraceData unless given
#   SEED, STEPS, FRIENDS   with WALK: the walk's seed, step count and number of friends (label_walk: of remotes)
#   FAULTY      with WALK: send walks the test-only model with a faulty send, queue the one with a faulty queue,
#               move the one whose move_tab leaves active behind, queue-cap the one whose queue drops its oldest past
#               four, send-trim the one whose send strips trailing spaces, send-dedup the one whose send of the
#               text just sent appends nothing, close the one whose close_tab removes a friend, sprint-move the one
#               whose move_tab puts the sprint tab among the friends, sprint-close the one whose close_tab removes the
#               sprint tab, close-fallback the one whose close of the active tab ignores the selection history;
#               for label_walk, before the one that takes the label to return to at a session's first observation
#   NOSPRINT    with WALK: ON walks a model made without the sprint tab
#   LINKWALK    optional, in place of WALK: tests/sync's link_walk program; it is run first and writes
#               LinkTraceData.tla into WORK, with SEED, STEPS and WINDOWS (its windows), and FAULTY one of applier,
#               mirror, tail
#   OVERLAYWALK optional, in place of WALK: tests/model's overlay_walk program; it is run first and writes
#               OverlayTraceData.tla into WORK, with SEED and STEPS, and FAULTY one of hide-unkeyed, hotkey-hides
#   NOCHECK_DEADLOCK  optional: ON passes -deadlock (TLC then does not report deadlock)
#   WORKERS     optional: TLC's worker count (-workers), 2 unless given
#   CAP         optional: the cap on the TLC run in seconds, 60 unless given
#
# The TLC jar is $ENV{TLA2TOOLS_JAR}; java is $ENV{JAVA}, else `java` on PATH. Each TLC run is capped at CAP seconds.

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
file(GLOB specs "${MODELS}/*.tla" "${MODELS}/*.cfg" "${MODELS}/witnesses/*.tla" "${MODELS}/witnesses/*.cfg")
file(COPY ${specs} DESTINATION "${WORK}")
get_filename_component(cfg_name "${CFG}" NAME)

if(DEFINED LINKWALK AND NOT "${LINKWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}" "${WINDOWS}")
    if(FAULTY STREQUAL "applier" OR FAULTY STREQUAL "mirror" OR FAULTY STREQUAL "tail")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not applier, mirror or tail")
    endif()
    execute_process(COMMAND "${LINKWALK}" ${walk_args} "${WORK}/LinkTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "link_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED OVERLAYWALK AND NOT "${OVERLAYWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "hide-unkeyed" OR FAULTY STREQUAL "hotkey-hides")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not hide-unkeyed or hotkey-hides")
    endif()
    execute_process(COMMAND "${OVERLAYWALK}" ${walk_args} "${WORK}/OverlayTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "overlay_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED WALK AND NOT "${WALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}" "${FRIENDS}")
    if(FAULTY STREQUAL "send")
        list(APPEND walk_args --faulty)
    elseif(FAULTY STREQUAL "queue")
        list(APPEND walk_args --faulty-queue)
    elseif(FAULTY STREQUAL "move")
        list(APPEND walk_args --faulty-move)
    elseif(FAULTY STREQUAL "queue-cap")
        list(APPEND walk_args --faulty-queue-cap)
    elseif(FAULTY STREQUAL "send-trim")
        list(APPEND walk_args --faulty-send-trim)
    elseif(FAULTY STREQUAL "send-dedup")
        list(APPEND walk_args --faulty-send-dedup)
    elseif(FAULTY STREQUAL "close")
        list(APPEND walk_args --faulty-close)
    elseif(FAULTY STREQUAL "sprint-move")
        list(APPEND walk_args --faulty-sprint-move)
    elseif(FAULTY STREQUAL "sprint-close")
        list(APPEND walk_args --faulty-sprint-close)
    elseif(FAULTY STREQUAL "close-fallback")
        list(APPEND walk_args --faulty-close-fallback)
    elseif(FAULTY STREQUAL "before")
        list(APPEND walk_args --faulty-before)
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not send, queue, move, queue-cap, send-trim, send-dedup, "
            "close, sprint-move, sprint-close, close-fallback or before")
    endif()
    if(NOSPRINT)
        list(APPEND walk_args --no-sprint)
    endif()
    if(NOT DEFINED DATA OR "${DATA}" STREQUAL "")
        set(DATA WindowTraceData)
    endif()
    execute_process(COMMAND "${WALK}" ${walk_args} "${WORK}/${DATA}.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "trace_walk failed (${walk_rc}): ${walk_out}")
    endif()
endif()

if(NOT DEFINED WORKERS OR "${WORKERS}" STREQUAL "")
    set(WORKERS 2)
endif()
if(NOT DEFINED CAP OR "${CAP}" STREQUAL "")
    set(CAP 60)
endif()
# The JVM's own threads, sized to the run. Left alone, each JVM starts a garbage collector thread for most cores and
# several compiler threads, and ctest runs one JVM per core: on a busy machine those threads, not TLC, use the cores.
if(WORKERS GREATER 2)
    set(jvm -XX:+UseParallelGC -XX:ParallelGCThreads=${WORKERS})
else()
    # A witness or a trace check runs for a second or two: the client compiler alone and the serial collector.
    set(jvm -XX:+UseSerialGC -XX:TieredStopAtLevel=1)
endif()
set(flags -workers ${WORKERS} -metadir "${WORK}/meta" -config "${cfg_name}")
if(NOCHECK_DEADLOCK)
    list(APPEND flags -deadlock)
endif()

execute_process(
    COMMAND "${java}" ${jvm} "-Djava.io.tmpdir=${WORK}/tmp" -cp "${jar}" tlc2.TLC ${flags} "${MODULE}.tla"
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
elseif(EXPECT STREQUAL "action")
    if(NOT out MATCHES "Action property ${PROPERTY} [^\n]*is violated")
        message(FATAL_ERROR "expected action property ${PROPERTY} to be violated, TLC exit ${rc}:\n${out}")
    endif()
elseif(EXPECT STREQUAL "liveness")
    if(NOT out MATCHES "Temporal properties were violated")
        message(FATAL_ERROR "expected a temporal property to be violated, TLC exit ${rc}:\n${out}")
    endif()
elseif(EXPECT STREQUAL "deadlock")
    if(NOT out MATCHES "Deadlock reached")
        message(FATAL_ERROR "expected a deadlock, TLC exit ${rc}:\n${out}")
    endif()
else()
    message(FATAL_ERROR "tlc.cmake: EXPECT is ${EXPECT}, not pass, invariant, action, liveness or deadlock")
endif()
message("${MODULE} ${cfg_name}: as expected (${EXPECT} ${PROPERTY})")
