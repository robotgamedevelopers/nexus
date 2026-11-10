# Tests of the window model (src/model.h), with three labels:
#   unit           model_test: the model's behaviour and edges, logic only
#   model          TLC: Window.tla on four instances, every reversed witness rejected, code traces checked against the
#                  spec over several seeds, and a faulty model's trace rejected. Needs TLA2TOOLS_JAR and java.
#   model_nightly  TLC: Window.tla on the three largest instances (the nightly workflow)

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

# tlc_test(<name> <module> <cfg> <expect> [PROPERTY p] [SEED s STEPS n FRIENDS f] [FAULTY <fault>] [NOSPRINT]
#          [WORKERS w] [NIGHTLY])
# WORKERS is TLC's worker count, 2 unless given; ctest reserves that many cores for the test (PROCESSORS), so
# `ctest -j <cores>` keeps one TLC worker per core.
# NIGHTLY labels the test model_nightly in place of model: the nightly workflow runs it, the per-commit model job does
# not (it runs `ctest -L '^model$'`). Its TLC run is capped at 300 s, not 60: see the instances below.
function(tlc_test name module cfg expect)
    cmake_parse_arguments(T "NOSPRINT;NIGHTLY" "PROPERTY;SEED;STEPS;FRIENDS;FAULTY;WORKERS" "" ${ARGN})
    if(NOT T_WORKERS)
        set(T_WORKERS 2)
    endif()
    set(label model)
    set(cap 60)
    if(T_NIGHTLY)
        set(label model_nightly)
        set(cap 300)
    endif()
    math(EXPR ctest_timeout "${cap} + 30")
    set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${models_dir}
             -DMODULE=${module} -DCFG=${cfg} -DEXPECT=${expect} -DWORKERS=${T_WORKERS} -DCAP=${cap})
    if(T_PROPERTY)
        list(APPEND defs -DPROPERTY=${T_PROPERTY})
    endif()
    if(DEFINED T_SEED)
        list(APPEND defs -DWALK=$<TARGET_FILE:trace_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS} -DFRIENDS=${T_FRIENDS})
    endif()
    if(T_FAULTY)
        list(APPEND defs -DFAULTY=${T_FAULTY})
    endif()
    if(T_NOSPRINT)
        list(APPEND defs -DNOSPRINT=ON)
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${tlc_script})
    set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT ${ctest_timeout} PROCESSORS ${T_WORKERS})
endfunction()

# The spec, every property, no error. The instances with tens of thousands of states get eight workers: TLC's time on
# them falls nearly in proportion (MCWindow3.cfg 28 s on two workers, 8 s on eight, on the studio at full speed).
tlc_test(tlc_spec_sprint_terminals MCWindow MCWindowSprint.cfg pass WORKERS 8)
tlc_test(tlc_spec_no_friends MCWindow MCWindowEmpty.cfg pass)
tlc_test(tlc_spec_no_sprint  MCWindow MCWindowNoSprint.cfg pass WORKERS 8)
tlc_test(tlc_spec_three_behind_sprint MCWindow MCWindow3Sprint.cfg pass WORKERS 8)
# Nightly: the three largest instances. With the selection history, every order of it a state of its own: the sprint tab,
# two friends, two terminals and entries of one, 320,760 states (37 s on eight workers in a login session; its form
# without entries, MCWindowSprint.cfg, 8,910, runs above); three friends in all orders, 384,912 (27 s); entries and
# queues of two, 214,326 (12 s).
# The per-commit job checks every property on the instances above, every witness and every trace. On the studio's CI
# runners TLC runs about four times slower than in a login session (MCWindow3.cfg on eight workers, before the history:
# 8 s in a login session, 31 to 38 s at the utility QoS, which reproduces the CI runs' timings), so these do not fit a
# 60 s cap. 300 s still stops a state space that runs away.
tlc_test(tlc_spec_two_tabs   MCWindow MCWindow.cfg      pass WORKERS 8 NIGHTLY)
tlc_test(tlc_spec_three_tabs MCWindow MCWindow3.cfg     pass WORKERS 8 NIGHTLY)
tlc_test(tlc_spec_long       MCWindow MCWindowLong.cfg  pass WORKERS 8 NIGHTLY)

# Reversed witnesses: each broken variant must be rejected by the property named.
tlc_test(tlc_witness_send_clears_all      WSendClearsAll      WSendClearsAll.cfg      action    PROPERTY SendClearsOnlyItsDraft)
tlc_test(tlc_witness_send_to_active       WSendToActive       WSendToActive.cfg       action    PROPERTY SendTouchesNoOtherTab)
tlc_test(tlc_witness_select_out_of_range  WSelectOutOfRange   WSelectOutOfRange.cfg   invariant PROPERTY ActiveValid)
tlc_test(tlc_witness_send_whitespace      WSendWhitespace     WSendWhitespace.cfg     action    PROPERTY SendResult)
tlc_test(tlc_witness_rewrite_entry        WRewriteEntry       WRewriteEntry.cfg       action    PROPERTY TranscriptsOnlyGrow)
tlc_test(tlc_witness_refused_clears_draft WRefusedClearsDraft WRefusedClearsDraft.cfg action    PROPERTY RefusedChangesNothing)
tlc_test(tlc_witness_queue_to_transcript  WQueueToTranscript  WQueueToTranscript.cfg  action    PROPERTY QueueLeavesTranscripts)
tlc_test(tlc_witness_queue_wrong_tab      WQueueWrongTab      WQueueWrongTab.cfg      action    PROPERTY SendTouchesNoOtherTab)
tlc_test(tlc_witness_send_also_queues     WSendAlsoQueues     WSendAlsoQueues.cfg     action    PROPERTY OnlyQueueTouchesQueues)
tlc_test(tlc_witness_queue_prepends       WQueuePrepends      WQueuePrepends.cfg      action    PROPERTY QueuesOnlyGrow)
tlc_test(tlc_witness_append_drops_queued  WAppendDropsQueued  WAppendDropsQueued.cfg  action    PROPERTY QueuesOnlyGrow)
tlc_test(tlc_witness_set_draft_active_too WSetDraftActiveToo  WSetDraftActiveToo.cfg  action    PROPERTY SetDraftExact)
tlc_test(tlc_witness_select_clears_draft  WSelectClearsDraft  WSelectClearsDraft.cfg  action    PROPERTY SelectExact)
tlc_test(tlc_witness_append_wrong_tab     WAppendWrongTab     WAppendWrongTab.cfg     action    PROPERTY AppendExact)
tlc_test(tlc_witness_move_drops_tab       WMoveDropsTab       WMoveDropsTab.cfg       invariant PROPERTY NoTabLost)
tlc_test(tlc_witness_move_duplicates_tab  WMoveDuplicatesTab  WMoveDuplicatesTab.cfg  invariant PROPERTY NoTabDuplicated)
tlc_test(tlc_witness_move_leaves_active   WMoveLeavesActive   WMoveLeavesActive.cfg   action    PROPERTY ActiveFriendKept)
tlc_test(tlc_witness_move_leaves_draft    WMoveLeavesDraft    WMoveLeavesDraft.cfg    action    PROPERTY MoveKeepsTabs)
tlc_test(tlc_witness_move_out_of_range    WMoveOutOfRange     WMoveOutOfRange.cfg     action    PROPERTY RefusedChangesNothing)
tlc_test(tlc_witness_move_swaps           WMoveSwaps          WMoveSwaps.cfg          action    PROPERTY MovePlaces)
tlc_test(tlc_witness_send_by_position     WSendByPosition     WSendByPosition.cfg     action    PROPERTY SendTouchesNoOtherTab)
tlc_test(tlc_witness_set_draft_outside    WSetDraftOutsideDrafts WSetDraftOutsideDrafts.cfg invariant PROPERTY TypeOK)
tlc_test(tlc_witness_send_appends_twice   WSendAppendsTwice   WSendAppendsTwice.cfg   action    PROPERTY SendAppendsOne)
tlc_test(tlc_witness_queue_appends_twice  WQueueAppendsTwice  WQueueAppendsTwice.cfg  action    PROPERTY QueueAppendsOne)
tlc_test(tlc_witness_select_reorders      WSelectReorders     WSelectReorders.cfg     action    PROPERTY OnlyMoveReorders)
tlc_test(tlc_witness_append_renames       WAppendRenamesFriend WAppendRenamesFriend.cfg action  PROPERTY FriendsFixed)
# Terminal tabs: added, closed, and kept in their zone after the friends.
tlc_test(tlc_witness_close_removes_friend WCloseRemovesFriend WCloseRemovesFriend.cfg invariant PROPERTY NoTabLost)
tlc_test(tlc_witness_close_wrong_tab      WCloseWrongTab      WCloseWrongTab.cfg      action    PROPERTY CloseRemovesExactly)
tlc_test(tlc_witness_close_active_past_end WCloseActivePastEnd WCloseActivePastEnd.cfg invariant PROPERTY ActiveValid)
tlc_test(tlc_witness_close_active_wrong   WCloseActiveWrong   WCloseActiveWrong.cfg   action    PROPERTY CloseActiveRule)
tlc_test(tlc_witness_close_drops_draft    WCloseDropsDraft    WCloseDropsDraft.cfg    action    PROPERTY CloseKeepsFriends)
tlc_test(tlc_witness_add_reuses_id        WAddReusesId        WAddReusesId.cfg        action    PROPERTY AddFreshId)
tlc_test(tlc_witness_add_among_terminals  WAddAmongTerminals  WAddAmongTerminals.cfg  action    PROPERTY AddAppends)
tlc_test(tlc_witness_add_before_friend    WAddBeforeFriend    WAddBeforeFriend.cfg    invariant PROPERTY ZonesOrdered)
tlc_test(tlc_witness_send_to_terminal     WSendToTerminal     WSendToTerminal.cfg     action    PROPERTY TerminalTakesNoText)
tlc_test(tlc_witness_move_terminal_before_friend WMoveTerminalBeforeFriend WMoveTerminalBeforeFriend.cfg invariant
         PROPERTY ZonesOrdered)
tlc_test(tlc_witness_move_friend_behind_terminal WMoveFriendBehindTerminal WMoveFriendBehindTerminal.cfg invariant
         PROPERTY ZonesOrdered)
tlc_test(tlc_witness_move_cross_zone_moves WMoveCrossZoneMoves WMoveCrossZoneMoves.cfg action    PROPERTY RefusedChangesNothing)
# The sprint tab: always first, never moved, never closed, never takes text.
tlc_test(tlc_witness_move_before_sprint   WMoveBeforeSprint   WMoveBeforeSprint.cfg   invariant PROPERTY SprintFirst)
tlc_test(tlc_witness_move_sprint          WMoveSprint         WMoveSprint.cfg         action    PROPERTY SprintNeverMoves)
tlc_test(tlc_witness_close_sprint         WCloseSprint        WCloseSprint.cfg        action    PROPERTY SprintNeverCloses)
tlc_test(tlc_witness_send_to_sprint       WSendToSprint       WSendToSprint.cfg       action    PROPERTY SprintTakesNoText)
# The selection history: closing the active tab goes back to the tab selected before it.
tlc_test(tlc_witness_close_old_rule       WCloseOldRule       WCloseOldRule.cfg       action    PROPERTY CloseActiveRule)
tlc_test(tlc_witness_close_keeps_history  WCloseKeepsHistory  WCloseKeepsHistory.cfg  invariant PROPERTY HistoryOpen)
tlc_test(tlc_witness_close_forgets_all    WCloseForgetsAll    WCloseForgetsAll.cfg    action    PROPERTY CloseForgets)
tlc_test(tlc_witness_select_keeps_place   WSelectKeepsPlace   WSelectKeepsPlace.cfg   action    PROPERTY SelectMovesToEnd)
tlc_test(tlc_witness_select_appends_again WSelectAppendsAgain WSelectAppendsAgain.cfg invariant PROPERTY HistoryOnce)
tlc_test(tlc_witness_select_skips_history WSelectSkipsHistory WSelectSkipsHistory.cfg invariant PROPERTY HistoryEndsActive)

# Trace binding: the real model's random walks, each a behaviour of Window.tla. 1000 steps take every walk's queues past
# four prompts (trace_walk prints the longest).
foreach(seed 1 2 3 4 5)
    tlc_test(tlc_trace_seed_${seed} WindowTrace WindowTrace.cfg pass SEED ${seed} STEPS 1000 FRIENDS 3)
endforeach()
tlc_test(tlc_trace_two_tabs WindowTrace WindowTrace.cfg pass SEED 6 STEPS 1000 FRIENDS 2)
tlc_test(tlc_trace_one_tab  WindowTrace WindowTrace.cfg pass SEED 8 STEPS 1000 FRIENDS 1)
tlc_test(tlc_trace_no_tabs  WindowTrace WindowTrace.cfg pass SEED 7 STEPS 100 FRIENDS 0)
# Every walk above starts with the sprint tab, as the window does; this one is the model without it.
tlc_test(tlc_trace_no_sprint WindowTrace WindowTrace.cfg pass SEED 9 STEPS 1000 FRIENDS 3 NOSPRINT)

# The binding can fail: a faulty model's trace must be rejected, with the fault in send, in queue and in move.
tlc_test(tlc_trace_faulty_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 300 FRIENDS 3 FAULTY send)
tlc_test(tlc_trace_faulty_queue_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 1000 FRIENDS 3 FAULTY queue)
tlc_test(tlc_trace_faulty_move_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 300 FRIENDS 3 FAULTY move)
# And with the queue dropping its oldest prompt past four, and send stripping trailing spaces.
tlc_test(tlc_trace_faulty_queue_cap_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 1000 FRIENDS 3 FAULTY queue-cap)
tlc_test(tlc_trace_faulty_send_trim_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 1000 FRIENDS 3 FAULTY send-trim)
# And with a send of the text just sent appending nothing (a hole the walk had before it sent equal text twice).
tlc_test(tlc_trace_faulty_send_dedup_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 1000 FRIENDS 3 FAULTY send-dedup)
# And with close removing a friend's tab.
tlc_test(tlc_trace_faulty_close_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 300 FRIENDS 3 FAULTY close)
# And with the sprint tab moved among the friends, and closed.
tlc_test(tlc_trace_faulty_sprint_move_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 300 FRIENDS 3 FAULTY sprint-move)
tlc_test(tlc_trace_faulty_sprint_close_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 300 FRIENDS 3 FAULTY sprint-close)
# And with closing the active tab ignoring the selection history.
tlc_test(tlc_trace_faulty_close_fallback_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 300 FRIENDS 3 FAULTY close-fallback)
