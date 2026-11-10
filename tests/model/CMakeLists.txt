# Tests of the window model (src/model.h), with two labels:
#   unit   model_test: the model's behaviour and edges, logic only
#   model  TLC: Window.tla on three instances, every reversed witness rejected, code traces checked against the
#          spec over several seeds, and a faulty model's trace rejected. Needs TLA2TOOLS_JAR and java.

set(warnings -Wall -Wextra -Wpedantic -Werror)

add_executable(model_test model_test.cpp)
target_link_libraries(model_test PRIVATE nexus_model)
target_compile_options(model_test PRIVATE ${warnings})

add_executable(trace_walk trace_walk.cpp)
target_link_libraries(trace_walk PRIVATE nexus_model)
target_compile_options(trace_walk PRIVATE ${warnings})

# Every program the tests run, to build without the rest of the project: cmake --build <dir> --target model_checks
add_custom_target(model_checks DEPENDS model_test trace_walk)

add_test(NAME model_unit COMMAND model_test)
set_tests_properties(model_unit PROPERTIES LABELS unit TIMEOUT 30)

set(models_dir "${PROJECT_SOURCE_DIR}/models")
set(tlc_script "${CMAKE_CURRENT_SOURCE_DIR}/tlc.cmake")

# tlc_test(<name> <module> <cfg> <expect> [PROPERTY p] [SEED s STEPS n FRIENDS f] [FAULTY])
function(tlc_test name module cfg expect)
    cmake_parse_arguments(T "FAULTY" "PROPERTY;SEED;STEPS;FRIENDS" "" ${ARGN})
    set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${models_dir}
             -DMODULE=${module} -DCFG=${cfg} -DEXPECT=${expect})
    if(T_PROPERTY)
        list(APPEND defs -DPROPERTY=${T_PROPERTY})
    endif()
    if(DEFINED T_SEED)
        list(APPEND defs -DWALK=$<TARGET_FILE:trace_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS} -DFRIENDS=${T_FRIENDS})
    endif()
    if(T_FAULTY)
        list(APPEND defs -DFAULTY=ON)
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${tlc_script})
    set_tests_properties(${name} PROPERTIES LABELS model TIMEOUT 90)
endfunction()

# The spec, every property, no error.
tlc_test(tlc_spec_two_tabs   MCWindow MCWindow.cfg      pass)
tlc_test(tlc_spec_three_tabs MCWindow MCWindow3.cfg     pass)
tlc_test(tlc_spec_no_tabs    MCWindow MCWindowEmpty.cfg pass)

# Reversed witnesses: each broken variant must be rejected by the property named.
tlc_test(tlc_witness_send_clears_all      WSendClearsAll      WSendClearsAll.cfg      action    PROPERTY SendClearsOnlyItsDraft)
tlc_test(tlc_witness_send_to_active       WSendToActive       WSendToActive.cfg       action    PROPERTY SendTouchesNoOtherTab)
tlc_test(tlc_witness_select_out_of_range  WSelectOutOfRange   WSelectOutOfRange.cfg   invariant PROPERTY ActiveValid)
tlc_test(tlc_witness_send_whitespace      WSendWhitespace     WSendWhitespace.cfg     action    PROPERTY SendResult)
tlc_test(tlc_witness_rewrite_entry        WRewriteEntry       WRewriteEntry.cfg       action    PROPERTY TranscriptsOnlyGrow)
tlc_test(tlc_witness_refused_clears_draft WRefusedClearsDraft WRefusedClearsDraft.cfg action    PROPERTY RefusedChangesNothing)
tlc_test(tlc_witness_queue_as_person      WQueueAsPerson      WQueueAsPerson.cfg      action    PROPERTY SendAppendsOne)
tlc_test(tlc_witness_set_draft_active_too WSetDraftActiveToo  WSetDraftActiveToo.cfg  action    PROPERTY SetDraftExact)
tlc_test(tlc_witness_select_clears_draft  WSelectClearsDraft  WSelectClearsDraft.cfg  action    PROPERTY SelectExact)
tlc_test(tlc_witness_append_wrong_tab     WAppendWrongTab     WAppendWrongTab.cfg     action    PROPERTY AppendExact)

# Trace binding: the real model's random walks, each a behaviour of Window.tla.
foreach(seed 1 2 3 4 5)
    tlc_test(tlc_trace_seed_${seed} WindowTrace WindowTrace.cfg pass SEED ${seed} STEPS 300 FRIENDS 3)
endforeach()
tlc_test(tlc_trace_two_tabs WindowTrace WindowTrace.cfg pass SEED 6 STEPS 300 FRIENDS 2)
tlc_test(tlc_trace_no_tabs  WindowTrace WindowTrace.cfg pass SEED 7 STEPS 100 FRIENDS 0)

# The binding can fail: a faulty model's trace must be rejected.
tlc_test(tlc_trace_faulty_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 300 FRIENDS 3 FAULTY)
