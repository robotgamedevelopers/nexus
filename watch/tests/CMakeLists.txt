# The watch core's tests (docs/watch.md), with two labels:
#   unit    watch_core_test, one test per group: logic only, no clock, sockets or files
#   model   TLC on watch/models: VoiceTurn.tla and Relay.tla on small instances, every reversed witness rejected, and
#           the code's walks (voice_walk, relay_walk) checked against them over several seeds, with each walk's planted
#           fault rejected. Needs java and TLA2TOOLS_JAR (the pinned tla2tools.jar, .github/tla2tools.sha256).

add_executable(watch_core_test core_test.cpp)
target_link_libraries(watch_core_test PRIVATE nexus_watch_core)
target_compile_options(watch_core_test PRIVATE ${NEXUS_WARNINGS})
foreach(group match turn frames watchlink phone capi screen)
    add_test(NAME watch_unit_${group} COMMAND watch_core_test ${group})
    set_tests_properties(watch_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()

# The walks TLC checks the code's traces with, over a copy of the core built with its planted faults
# (NEXUS_WATCH_FAULTS: voice_turn.h); no other build has them.
add_library(nexus_watch_core_faulty STATIC ../core/ai_match.cpp ../core/voice_turn.cpp ../core/relay.cpp
    ${NEXUS_WATCH_SRC}/voice/name_match.cpp)
target_include_directories(nexus_watch_core_faulty PUBLIC ../core PRIVATE ${NEXUS_WATCH_SRC})
target_compile_definitions(nexus_watch_core_faulty PUBLIC NEXUS_WATCH_FAULTS)
target_compile_options(nexus_watch_core_faulty PRIVATE ${NEXUS_WARNINGS})
add_executable(watch_voice_walk voice_walk.cpp)
target_link_libraries(watch_voice_walk PRIVATE nexus_watch_core_faulty)
target_compile_options(watch_voice_walk PRIVATE ${NEXUS_WARNINGS})
add_executable(watch_relay_walk relay_walk.cpp)
target_link_libraries(watch_relay_walk PRIVATE nexus_watch_core_faulty)
target_compile_options(watch_relay_walk PRIVATE ${NEXUS_WARNINGS})
add_custom_target(watch_model_checks DEPENDS watch_core_test watch_voice_walk watch_relay_walk)

find_program(NEXUS_WATCH_JAVA java)
set(NEXUS_WATCH_TLC_JAR "$ENV{TLA2TOOLS_JAR}" CACHE FILEPATH "The pinned tla2tools.jar")
set(models "${CMAKE_CURRENT_SOURCE_DIR}/../models")

# watch_tlc(<name> <module> <cfg> <expect> [WALK <target> SEED <s> STEPS <n> DATA <module> [FAULT <f>]])
function(watch_tlc name module cfg expect)
    cmake_parse_arguments(T "" "WALK;SEED;STEPS;DATA;FAULT" "" ${ARGN})
    set(walk_args)
    if(T_WALK)
        set(walk_args "-DWALK=$<TARGET_FILE:${T_WALK}>" "-DSEED=${T_SEED}" "-DSTEPS=${T_STEPS}" "-DDATA=${T_DATA}"
            "-DFAULT=${T_FAULT}")
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} "-DJAVA=${NEXUS_WATCH_JAVA}" "-DJAR=${NEXUS_WATCH_TLC_JAR}"
        "-DMODELS=${models}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name}" "-DMODULE=${module}" "-DCFG=${cfg}"
        "-DEXPECT=${expect}" ${walk_args} -P "${CMAKE_CURRENT_SOURCE_DIR}/tlc.cmake")
    set_tests_properties(${name} PROPERTIES LABELS model TIMEOUT 120 PROCESSORS 2)
endfunction()

watch_tlc(tlc_watchapp_model_voice VoiceTurn MCVoiceTurn.cfg pass)
watch_tlc(tlc_watchapp_model_relay Relay MCRelay.cfg pass)
file(GLOB witnesses RELATIVE "${models}/witnesses" "${models}/witnesses/W*.cfg")
foreach(cfg ${witnesses})
    get_filename_component(w "${cfg}" NAME_WE)
    file(STRINGS "${models}/witnesses/${cfg}" must REGEX "^\\\\\\* Must fail: (invariant|property) ")
    string(REGEX REPLACE "^.* ([A-Za-z]+)\\.$" "\\1" property "${must}")
    watch_tlc(tlc_watchapp_witness_${w} ${w} ${cfg} fail:${property})
endforeach()
foreach(seed 1 2 3 4)
    watch_tlc(tlc_watchapp_trace_voice_${seed} VoiceTurnTrace VoiceTurnTrace.cfg pass
        WALK watch_voice_walk SEED ${seed} STEPS 400 DATA VoiceTurnTraceData)
    watch_tlc(tlc_watchapp_trace_relay_${seed} RelayTrace RelayTrace.cfg pass
        WALK watch_relay_walk SEED ${seed} STEPS 400 DATA RelayTraceData)
endforeach()
foreach(fault silent-mic unread drop-on-back speak-locked listen-locked)
    watch_tlc(tlc_watchapp_trace_voice_fault_${fault} VoiceTurnTrace VoiceTurnTrace.cfg fail:Trace
        WALK watch_voice_walk SEED 1 STEPS 1500 DATA VoiceTurnTraceData FAULT ${fault})
endforeach()
foreach(fault forget-handed ack-early vouch-all)
    watch_tlc(tlc_watchapp_trace_relay_fault_${fault} RelayTrace RelayTrace.cfg fail:Trace
        WALK watch_relay_walk SEED 1 STEPS 400 DATA RelayTraceData FAULT ${fault})
endforeach()
