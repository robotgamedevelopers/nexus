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
#   SEED, STEPS, AIS   with WALK: the walk's seed, step count and number of AIs (label_walk: of remotes)
#   FAULTY      with WALK: send walks the test-only model with a faulty send, queue the one with a faulty queue,
#               move the one whose move_tab leaves active behind, queue-cap the one whose queue drops its oldest past
#               four, send-trim the one whose send strips trailing spaces, send-dedup the one whose send of the
#               text just sent appends nothing, close the one whose close_tab removes an AI, sprint-move the one
#               whose move_tab puts the sprint tab among the AIs, sprint-close the one whose close_tab removes the
#               sprint tab, close-fallback the one whose close of the active tab ignores the selection history,
#               remove-ai (with CHURN) the one whose remove_ai leaves the tab's id in the history;
#               for label_walk, before the one that takes the label to return to at a session's first observation
#   NOSPRINT    with WALK: ON walks a model made without the sprint tab
#   CHURN       with WALK: ON walks the AIs coming and going (trace_walk --ais-churn)
#   LINKWALK    optional, in place of WALK: tests/sync's link_walk program; it is run first and writes
#               LinkTraceData.tla into WORK, with SEED, STEPS and WINDOWS (its windows), and FAULTY one of applier,
#               mirror, tail, close or ensure
#   STRIPWALK   optional, in place of WALK: strip_walk; it is run first and writes AIStripTraceData.tla into WORK,
#               with SEED, STEPS and AIS (2 or 3), and FAULTY one of forget, wait
#   GLIDEWALK   optional, in place of WALK: glide_walk; it is run first and writes GlideTraceData.tla into WORK, with
#               SEED and STEPS, and FAULTY keyup for the scroller that loses an arrow's release
#   SUPWALK     optional, in place of WALK: tests/server's supervisor_walk; it is run first and writes
#               SupervisorTraceData.tla into WORK, with SEED and STEPS, FAULTY one of turn, resume, queue,
#               limit, setting, account, activity, dir, remove or unknown (both with CHURN), and CHURN ON for the
#               AI taken out of ais.json and put back (supervisor_walk --ais-churn)
#   DRIVERWALK  optional, in place of WALK: the Claude driver's walk (drivers/claude/dist/test/walk.js), run under
#               NODE; it is run first and writes ClaudeDriverTraceData.tla into WORK, with SEED and STEPS, and FAULTY
#               one of settings, refusal, latch
#   DRIVERTESTS optional, in place of WALK: the Claude driver's unit tests (drivers/claude/dist/test/unit/
#               driver.test.js), run under NODE with NEXUS_DRIVER_TRACE set; they write ClaudeDriverTraceData.tla
#               into WORK, the traces of every test that keeps to the model
#   NODE        with DRIVERWALK or DRIVERTESTS: node (Node 20.19 or newer)
#   SECWALK     optional, in place of WALK: tests/sync's secrets_walk; it is run first and writes
#               SecretsTraceData.tla into WORK, with SEED and STEPS, FORMAT (3, 4 or 5, the windows' and actions'
#               format; 3 unless given), and FAULTY one of store-late, echo, driver, driver-echo
#   SIGNWALK    optional, in place of WALK: tests/sync's signin_walk; it is run first and writes
#               SignInTraceData.tla into WORK, with SEED and STEPS, and FAULTY one of claim, callback, stray
#   ACCWALK     optional, in place of WALK: tests/accounts' accounts_walk; it is run first and writes
#               AccountsTraceData.tla into WORK, with SEED and STEPS, and FAULTY one of stay, harness, instance, limit
#   RECONNECTWALK optional, in place of WALK: tests/sync's reconnect_walk; it is run first and writes
#               ReconnectTraceData.tla into WORK, with SEED and STEPS, and FAULTY one of cached, no-reset
#   RELAYWALK   optional, in place of WALK: tests/relay's relay_walk; it is run first and writes
#               RelayLifeTraceData.tla into WORK, with SEED and STEPS, and FAULTY one of resend, early-end, ack-delivered
#   OVERLAYWALK optional, in place of WALK: tests/model's overlay_walk program; it is run first and writes
#               OverlayTraceData.tla into WORK, with SEED and STEPS, and FAULTY one of hide-unkeyed, hotkey-hides
#   LOCKWALK    optional, in place of WALK: tests/model's lock_walk program; it is run first and writes
#               ClientLockTraceData.tla into WORK, with SEED and STEPS, and FAULTY one of stale, draw, idle
#   TSWALK      optional, in place of WALK: the Tailscale shim's lifecycle module (src/tailscale/lifecycle), whose walk
#               (./walk) runs under GO, offline, with the local toolchain (the module needs no other); it is run
#               first and writes TailscaleTraceData.tla into WORK, with SEED and STEPS, and FAULTY one of ready, url,
#               close, table, down
#   GO          with TSWALK: a go command, 1.21 or newer
#   NOCHECK_DEADLOCK  optional: ON passes -deadlock (TLC then does not report deadlock)
#   WORKERS     optional: TLC's worker count (-workers), 2 unless given
#   CAP         optional: the cap on the TLC run in seconds, 60 unless given; 0 for none (NEXUS_TLC_UNCAPPED)
#   SPLIT       optional: rest:<name>, every property of the cfg but one, or as:<name>=<part>, a part of it alone (below)
#   TEMPORALCFG optional: append this cfg fragment, relative to MODELS, to an as:<name>=<part> check
#
# The TLC jar is $ENV{TLA2TOOLS_JAR}; java is $ENV{JAVA}, else `java` on PATH. Each TLC run is capped at CAP seconds,
# unless CAP is 0.

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

# SPLIT rest:<name>: check the instance with every property of its cfg but <name>. SPLIT as:<name>=<part>: check it with
# <part> in place of <name> and no other property. Every invariant either way. tlc_test's SPLIT declares the rest and
# one test per part, and the spec defines the parts so that their conjunction is the property (Supervisor.tla,
# SettlesOdd and SettlesEven), so between them the tests check every property of the cfg. A temporal part may supply
# a TEMPORALCFG that omits state unobserved by the property and the fair transition relation. A large instance's
# time is in its temporal property, not its states or action properties (MCSupervisorSwitch.cfg on four cores of a
# loaded 16-thread host, 2026-11-15: invariants and its 20 action properties 7.8 s, the temporal property Settles alone
# 67.9 s, all of them 122 s), and in each part less than half of it (one prompt's Settles 38.7 s, both 94.2 s),
# so the parts run side by side in CI shards of their own.
if(DEFINED SPLIT AND NOT "${SPLIT}" STREQUAL "")
    set(split_part "")
    if(SPLIT MATCHES "^rest:([A-Za-z_][A-Za-z0-9_]*)$")
        set(split_mode rest)
        set(split_names ${CMAKE_MATCH_1})
    elseif(SPLIT MATCHES "^as:([A-Za-z_][A-Za-z0-9_]*)=([A-Za-z_][A-Za-z0-9_]*)$")
        set(split_mode only)
        set(split_names ${CMAKE_MATCH_1})
        set(split_part ${CMAKE_MATCH_2})
    else()
        message(FATAL_ERROR "tlc.cmake: SPLIT is ${SPLIT}, not rest:<name> or as:<name>=<part>")
    endif()
    file(STRINGS "${WORK}/${cfg_name}" cfg_lines)
    set(in_properties FALSE)
    set(seen "")
    set(kept "")
    set(cfg_out "")
    foreach(line IN LISTS cfg_lines)
        if(line MATCHES "^[A-Z]")
            if(line MATCHES "^PROPERT(Y|IES)[ \t]*$")
                set(in_properties TRUE)
            elseif(line MATCHES "^PROPERT")
                message(FATAL_ERROR "tlc.cmake: SPLIT needs ${cfg_name}'s properties one to a line under PROPERTIES")
            else()
                set(in_properties FALSE)
            endif()
            string(APPEND cfg_out "${line}\n")
        elseif(in_properties AND line MATCHES "^[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*$")
            set(property ${CMAKE_MATCH_1})
            list(FIND split_names ${property} at)
            if(NOT at EQUAL -1)
                list(APPEND seen ${property})
            endif()
            if(split_mode STREQUAL "only" AND NOT at EQUAL -1)
                string(APPEND cfg_out "    ${split_part}\n")
                list(APPEND kept "${split_part} (in place of ${property})")
            elseif(split_mode STREQUAL "rest" AND at EQUAL -1)
                string(APPEND cfg_out "${line}\n")
                list(APPEND kept ${property})
            endif()
        elseif(in_properties AND line MATCHES "^[ \t]+[^ \t\\\\]")
            message(FATAL_ERROR "tlc.cmake: SPLIT needs ${cfg_name}'s properties one to a line; this one is not: ${line}")
        else()
            string(APPEND cfg_out "${line}\n")
        endif()
    endforeach()
    foreach(name IN LISTS split_names)
        list(FIND seen ${name} at)
        if(at EQUAL -1)
            message(FATAL_ERROR "tlc.cmake: SPLIT names ${name}, which is not among ${cfg_name}'s PROPERTIES")
        endif()
    endforeach()
    if(kept STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: SPLIT ${SPLIT} leaves ${cfg_name} with no property to check")
    endif()
    string(REPLACE ";" " " kept_text "${kept}")
    message("${cfg_name}, SPLIT ${SPLIT}: every invariant, and properties ${kept_text}")
    set(cfg_name "${split_mode}${split_part}-${cfg_name}")
    if(DEFINED TEMPORALCFG AND NOT "${TEMPORALCFG}" STREQUAL "")
        if(NOT split_mode STREQUAL "only")
            message(FATAL_ERROR "TEMPORALCFG is only valid on temporal-property shards")
        endif()
        file(READ "${MODELS}/${TEMPORALCFG}" extra)
        string(APPEND cfg_out "\n${extra}\n")
    endif()
    file(WRITE "${WORK}/${cfg_name}" "${cfg_out}")
endif()

if((DEFINED DRIVERWALK AND NOT "${DRIVERWALK}" STREQUAL "") OR (DEFINED DRIVERTESTS AND NOT "${DRIVERTESTS}" STREQUAL ""))
    if(NOT DEFINED NODE OR "${NODE}" STREQUAL "" OR NODE MATCHES "NOTFOUND$")
        message(FATAL_ERROR "No node on PATH when this build was configured. The Claude driver's trace checks run its "
            "walk and unit tests under Node: put Node 20.19 or newer on PATH (the fleet's sdk/bin) and configure again.")
    endif()
    set(program "${DRIVERWALK}${DRIVERTESTS}")
    if(NOT EXISTS "${program}")
        message(FATAL_ERROR "${program} does not exist: build the Claude driver first, with `npm ci && npm run build` "
            "in drivers/claude.")
    endif()
    if(DEFINED DRIVERWALK AND NOT "${DRIVERWALK}" STREQUAL "")
        set(walk_args "${DRIVERWALK}" "${SEED}" "${STEPS}")
        if(FAULTY STREQUAL "settings" OR FAULTY STREQUAL "refusal" OR FAULTY STREQUAL "latch" OR FAULTY STREQUAL "secret")
            list(APPEND walk_args "--faulty-${FAULTY}")
        elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
            message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not settings, refusal, latch or secret")
        endif()
        execute_process(COMMAND "${NODE}" ${walk_args} "${WORK}/ClaudeDriverTraceData.tla"
            RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "NEXUS_DRIVER_TRACE=${WORK}/ClaudeDriverTraceData.tla"
                "${NODE}" --test "${DRIVERTESTS}"
            WORKING_DIRECTORY "${WORK}"
            RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    endif()
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "the Claude driver's ${program} failed (${walk_rc}): ${walk_out}")
    endif()
    if(NOT EXISTS "${WORK}/ClaudeDriverTraceData.tla")
        message(FATAL_ERROR "${program} wrote no ClaudeDriverTraceData.tla into ${WORK}")
    endif()
elseif(DEFINED TSWALK AND NOT "${TSWALK}" STREQUAL "")
    if(NOT DEFINED GO OR "${GO}" STREQUAL "" OR GO MATCHES "NOTFOUND$")
        message(FATAL_ERROR "No go on PATH or in ~/sdk/bin when this build was configured. The Tailscale lifecycle's "
            "trace checks run its walk with Go 1.21 or newer (the fleet's sdk/bin has one): install it, configure again.")
    endif()
    set(walk_args "${SEED}" "${STEPS}" "${WORK}/TailscaleTraceData.tla")
    if(FAULTY STREQUAL "ready" OR FAULTY STREQUAL "url" OR FAULTY STREQUAL "close" OR FAULTY STREQUAL "table" OR
        FAULTY STREQUAL "down")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not ready, url, close, table or down")
    endif()
    # The module has no dependency: nothing is fetched (GOPROXY=off), and the local Go builds it (GOTOOLCHAIN=local),
    # with the user's own build cache (Go locks it, so parallel tests share it safely).
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env GOTOOLCHAIN=local GOPROXY=off GOWORK=off GOENV=off GOFLAGS=-mod=mod
            "${GO}" run ./walk ${walk_args}
        WORKING_DIRECTORY "${TSWALK}"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 60)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "the Tailscale lifecycle's walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED CREDITWALK)
    execute_process(COMMAND "${CREDITWALK}" --credit-handoff "${CREDITMODE}" "${WORK}/CreditHandoffData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "accounts_walk credit handoff failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED ACCWALK AND NOT "${ACCWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "stay" OR FAULTY STREQUAL "harness" OR FAULTY STREQUAL "instance" OR FAULTY STREQUAL "limit")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not stay, harness, instance or limit")
    endif()
    execute_process(COMMAND "${ACCWALK}" ${walk_args} "${WORK}/AccountsTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "accounts_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED RECONNECTWALK AND NOT "${RECONNECTWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "cached" OR FAULTY STREQUAL "no-reset")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not cached or no-reset")
    endif()
    execute_process(COMMAND "${RECONNECTWALK}" ${walk_args} "${WORK}/ReconnectTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "reconnect_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED RELAYWALK AND NOT "${RELAYWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "resend" OR FAULTY STREQUAL "early-end" OR FAULTY STREQUAL "ack-delivered")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not resend, early-end or ack-delivered")
    endif()
    execute_process(COMMAND "${RELAYWALK}" ${walk_args} "${WORK}/RelayLifeTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "relay_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED SECWALK AND NOT "${SECWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "store-late" OR FAULTY STREQUAL "echo" OR FAULTY STREQUAL "driver" OR
            FAULTY STREQUAL "driver-echo")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not store-late, echo, driver or driver-echo")
    endif()
    if(DEFINED FORMAT AND NOT FORMAT STREQUAL "")
        list(APPEND walk_args "--format" "${FORMAT}")
    endif()
    execute_process(COMMAND "${SECWALK}" ${walk_args} "${WORK}/SecretsTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "secrets_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED SIGNWALK AND NOT "${SIGNWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "claim" OR FAULTY STREQUAL "callback" OR FAULTY STREQUAL "stray")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not claim, callback or stray")
    endif()
    execute_process(COMMAND "${SIGNWALK}" ${walk_args} "${WORK}/SignInTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "signin_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED SUPWALK AND NOT "${SUPWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "turn" OR FAULTY STREQUAL "resume" OR FAULTY STREQUAL "queue" OR FAULTY STREQUAL "setting" OR
            FAULTY STREQUAL "account" OR FAULTY STREQUAL "activity" OR FAULTY STREQUAL "dir" OR
            FAULTY STREQUAL "limit" OR FAULTY STREQUAL "remove" OR FAULTY STREQUAL "unknown")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not turn, resume, queue, setting, account, activity, dir, "
            "limit, remove or unknown")
    endif()
    if(CHURN)
        list(APPEND walk_args "--ais-churn")
    endif()
    execute_process(COMMAND "${SUPWALK}" ${walk_args} "${WORK}/SupervisorTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "supervisor_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED STRIPWALK AND NOT "${STRIPWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}" "${AIS}")
    if(FAULTY STREQUAL "forget" OR FAULTY STREQUAL "wait")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not forget or wait")
    endif()
    execute_process(COMMAND "${STRIPWALK}" ${walk_args} "${WORK}/AIStripTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "strip_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED PASSWALK AND NOT "${PASSWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "release-before-flush" OR FAULTY STREQUAL "state-before-flush" OR
            FAULTY STREQUAL "tick-before-flush" OR FAULTY STREQUAL "no-hold" OR FAULTY STREQUAL "release-on-fail")
        list(APPEND walk_args "--fault" "${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not release-before-flush, state-before-flush, "
            "tick-before-flush, no-hold or release-on-fail")
    endif()
    execute_process(COMMAND "${PASSWALK}" ${walk_args} "${WORK}/LoopPassTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "pass_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED GLIDEWALK AND NOT "${GLIDEWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "keyup")
        list(APPEND walk_args --faulty-keyup)
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not keyup")
    endif()
    execute_process(COMMAND "${GLIDEWALK}" ${walk_args} "${WORK}/GlideTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "glide_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED LINKWALK AND NOT "${LINKWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}" "${WINDOWS}")
    if(FAULTY STREQUAL "applier" OR FAULTY STREQUAL "mirror" OR FAULTY STREQUAL "tail" OR FAULTY STREQUAL "close" OR
            FAULTY STREQUAL "ensure")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not applier, mirror, tail, close or ensure")
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
elseif(DEFINED LOCKWALK AND NOT "${LOCKWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "stale" OR FAULTY STREQUAL "draw" OR FAULTY STREQUAL "idle")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not stale, draw or idle")
    endif()
    execute_process(COMMAND "${LOCKWALK}" ${walk_args} "${WORK}/ClientLockTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "lock_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED WATCHWALK AND NOT "${WATCHWALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}")
    if(FAULTY STREQUAL "aba" OR FAULTY STREQUAL "stale")
        list(APPEND walk_args "--faulty-${FAULTY}")
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not aba or stale")
    endif()
    execute_process(COMMAND "${WATCHWALK}" ${walk_args} "${WORK}/TerminalWatchTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "watch_walk failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED BUSDELIVERYWALK)
    if(BUSPEERTRACE)
        set(bus_args --peer-roster "${WORK}/BusPeerFilterData.tla")
    else()
        set(bus_args "${WORK}/BusDeliveryData.tla")
    endif()
    execute_process(COMMAND "${BUSDELIVERYWALK}" ${bus_args}
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "bus_delivery_trace failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED SERVICEWALK)
    execute_process(COMMAND "${SERVICEWALK}" --trace "${WORK}/ManagedServiceTraceData.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "managed service trace failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED PICKERWALK)
    set(walk_args "${WORK}/AccountPickerTraceData.tla")
    if(FAULTY STREQUAL "cancel")
        list(APPEND walk_args --faulty-cancel)
    endif()
    execute_process(COMMAND "${PICKERWALK}" ${walk_args}
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "account_picker_trace failed (${walk_rc}): ${walk_out}")
    endif()
elseif(DEFINED WALK AND NOT "${WALK}" STREQUAL "")
    set(walk_args "${SEED}" "${STEPS}" "${AIS}")
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
    elseif(FAULTY STREQUAL "remove-ai")
        list(APPEND walk_args --faulty-remove-ai)
    elseif(FAULTY STREQUAL "before")
        list(APPEND walk_args --faulty-before)
    elseif(DEFINED FAULTY AND NOT FAULTY STREQUAL "")
        message(FATAL_ERROR "tlc.cmake: FAULTY is ${FAULTY}, not send, queue, move, queue-cap, send-trim, send-dedup, "
            "close, sprint-move, sprint-close, close-fallback, remove-ai or before")
    endif()
    if(NOSPRINT)
        list(APPEND walk_args --no-sprint)
    endif()
    if(CHURN)
        list(APPEND walk_args --ais-churn)
    endif()
    if(NOT DEFINED DATA OR "${DATA}" STREQUAL "")
        set(DATA WindowTraceData)
    endif()
    if(PLUGINTRACE)
        set(walk_args --plugin-tabs)
    endif()
    execute_process(COMMAND "${WALK}" ${walk_args} "${WORK}/${DATA}.tla"
        RESULT_VARIABLE walk_rc OUTPUT_VARIABLE walk_out ERROR_VARIABLE walk_out TIMEOUT 30)
    message("${walk_out}")
    if(NOT walk_rc EQUAL 0)
        message(FATAL_ERROR "trace_walk failed (${walk_rc}): ${walk_out}")
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
include("${CMAKE_CURRENT_LIST_DIR}/tlc-workers.cmake")
set(jvm ${tlc_jvm})
set(flags -workers ${tlc_workers} -metadir "${WORK}/meta" -config "${cfg_name}")
if(NOCHECK_DEADLOCK)
    list(APPEND flags -deadlock)
endif()

execute_process(
    COMMAND "${java}" ${jvm} "-Djava.io.tmpdir=${WORK}/tmp" -cp "${jar}" tlc2.TLC ${flags} "${MODULE}.tla"
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
