# Tests of the window model (src/model.h), with three labels:
#   unit           model_test: the model's behaviour and edges, logic only
#   model          TLC: Window.tla on four instances, every reversed witness rejected, code traces checked against the
#                  spec over several seeds, and a faulty model's trace rejected. Needs TLA2TOOLS_JAR and java.
#   model_nightly  TLC: Window.tla on the three largest instances (the nightly workflow)

set(warnings ${NEXUS_WARNINGS})

add_executable(model_test model_test.cpp)
target_link_libraries(model_test PRIVATE nexus_model)
target_compile_options(model_test PRIVATE ${warnings})

# The overlay's rules (src/overlay.h): their unit tests, and the walk whose traces OverlayTrace.tla checks.
add_executable(overlay_test overlay_test.cpp)
target_link_libraries(overlay_test PRIVATE nexus_model)
target_compile_options(overlay_test PRIVATE ${warnings})

# Every program the tests run, to build without the rest of the project: cmake --build <dir> --target model_checks
add_custom_target(model_checks DEPENDS model_test overlay_test)

# The walks whose traces TLC checks, which run on Linux and macOS only: a native Windows build has no TLC tests
# (tlc_test below) and so no walks (NEXUS_SERVER_PARTS in CMakeLists.txt).
if(NEXUS_SERVER_PARTS)
    add_executable(trace_walk trace_walk.cpp)
    target_link_libraries(trace_walk PRIVATE nexus_model)
    target_compile_options(trace_walk PRIVATE ${warnings})

    # TabLabel's walk (src/remote.h), for TabLabelTrace.tla: with the window's logic (NEXUS_WINDOW_LOGIC).
    if(NEXUS_WINDOW_LOGIC)
        add_executable(label_walk label_walk.cpp)
        target_link_libraries(label_walk PRIVATE nexus_window_logic)
        target_compile_options(label_walk PRIVATE ${warnings})
        add_dependencies(model_checks label_walk)
    endif()

    # The board scroller's walk (src/scroll_glide.h), for GlideTrace.tla.
    add_executable(glide_walk glide_walk.cpp)
    target_link_libraries(glide_walk PRIVATE nexus_scroll)
    target_compile_options(glide_walk PRIVATE ${warnings})

    add_executable(overlay_walk overlay_walk.cpp)
    target_link_libraries(overlay_walk PRIVATE nexus_model)
    target_compile_options(overlay_walk PRIVATE ${warnings})

    # The friend strip's walk (src/sync/friend_strip.h), for FriendStripTrace.tla.
    add_executable(strip_walk strip_walk.cpp)
    target_link_libraries(strip_walk PRIVATE nexus_sync)
    target_compile_options(strip_walk PRIVATE ${warnings})

    add_dependencies(model_checks trace_walk glide_walk overlay_walk strip_walk)
endif()

add_test(NAME model_unit COMMAND model_test)
set_tests_properties(model_unit PROPERTIES LABELS unit TIMEOUT 30)
add_test(NAME overlay_unit COMMAND overlay_test)
set_tests_properties(overlay_unit PROPERTIES LABELS unit TIMEOUT 30)

# tlc_test(<name> <module> <cfg> <expect> [PROPERTY p] [SEED s STEPS n FRIENDS f] [FAULTY <fault>] [NOSPRINT] [CHURN]
#          [SEED s STEPS n WINDOWS w LINKWALK] [SEED s STEPS n SUPWALK] [SEED s STEPS n ACCWALK]
#          [WORKERS w] [NIGHTLY] [SHARD] [CAP s] [SPLIT P=A+B] [LABEL] [OVERLAY])
# SPLIT P=A+B declares <name>_rest, every property of the cfg but P, and <name>_temporal_1 and _2, A and B in P's place,
# each with every invariant: the spec defines P as A /\ B, so a large instance's temporal property runs in parts beside
# the rest (tlc.cmake says why).
# WORKERS is TLC's worker count, 2 unless given; ctest reserves that many cores for the test (PROCESSORS), so
# `ctest -j <cores>` keeps one TLC worker per core; in CI a run takes no more workers than the runner's share of cores
# (NEXUS_TLC_CORES, tlc-workers.cmake). SHARD marks an instance that has a CI model shard of its own and
# caps it at that shard's budget (80 s; the function says how it is derived).
# NIGHTLY labels the test model_nightly in place of model: the nightly workflow runs it, the per-commit model job does
# not (it runs `ctest -L '^model$'`). Its TLC run is capped at 300 s, not 60: see the instances below. CAP s caps it at
# s instead, nightly or not; a per-commit cap stays well inside the CI job's 2 minutes (.github/workflows/ci.yml, the
# model shards, says how the per-commit caps and worker counts were measured).
# With SEED, the walk is trace_walk (the window's model, FRIENDS friends); with LINKWALK, it is tests/sync's link_walk
# (the link, WINDOWS windows), and FAULTY names one of its faults (applier, mirror, tail). Called from tests/sync too.
# With SEED and LABEL, the walk is label_walk (a terminal tab's label, FRIENDS remotes).
# With SEED and OVERLAY, the walk is overlay_walk (the overlay's rules, STEPS steps), and FAULTY names one of its faults
# (hide-unkeyed, hotkey-hides). With GLIDEWALK, it is glide_walk (the board's scroller), and FAULTY keyup its one fault.
# With STRIPWALK, it is strip_walk (the friend strip, FRIENDS friends), and FAULTY one of its faults (forget, wait).
# With SUPWALK, it is tests/server's supervisor_walk (a friend tab's driver supervisor), and FAULTY one of its faults
# (turn, resume, queue).
# With ACCWALK, it is tests/accounts' accounts_walk (the hub's accounts and friends), and FAULTY one of its faults
# (stay, harness). Called from tests/accounts.
# With RECONNECTWALK, it is tests/sync's reconnect_walk (the window's link making its session again), and FAULTY one of
# its faults (cached, no-reset). Called from tests/sync.
# With SECWALK, it is tests/sync's secrets_walk (the secret dialog: the hub's desk and the windows' prompts), and FAULTY
# one of its faults (store-late, echo). Called from tests/sync.
# With DRIVERWALK, it is the Claude driver's walk (drivers/claude/test/walk.ts, run under Node from its dist/), and
# FAULTY one of its faults (settings, refusal, latch); with DRIVERTESTS, the traces are those of the driver's unit
# tests (test/unit/driver.test.ts). Both need `npm ci && npm run build` in drivers/claude first. Called from
# tests/driver.
# With TSWALK, it is the Tailscale shim's lifecycle walk (src/tailscale/lifecycle/walk, run with the local Go: the
# module has no dependency), and FAULTY one of its faults (ready, url, close, table, down). NODEADLOCK checks a spec
# whose environment may come to rest (TLC's -deadlock).
find_program(NEXUS_NODE node)
find_program(NEXUS_GO_LOCAL go HINTS "$ENV{HOME}/sdk/bin")
function(tlc_test name module cfg expect)
    # TLC and the walks run on Linux and macOS; a native Windows build defines none of them (NEXUS_SERVER_PARTS).
    if(NOT NEXUS_SERVER_PARTS)
        return()
    endif()
    set(options NOSPRINT CHURN NIGHTLY LINKWALK LABEL OVERLAY GLIDEWALK SUPWALK ACCWALK SECWALK SIGNWALK RECONNECTWALK
        DRIVERWALK DRIVERTESTS STRIPWALK SHARD TSWALK NODEADLOCK)
    cmake_parse_arguments(T "${options}"
        "PROPERTY;SEED;STEPS;FRIENDS;WINDOWS;FAULTY;FORMAT;WORKERS;CAP;SPLIT;SPLITMODE" "" ${ARGN})
    # SPLIT P=A+B: <name>_rest checks every property of the cfg but P, and <name>_temporal_1, _2 check A and B in its
    # place, each with every invariant (tlc.cmake, SPLIT); the spec defines A /\ B as P, so between them every property
    # of the cfg is checked.
    if(T_SPLIT)
        if(NOT T_SPLIT MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=([A-Za-z_][A-Za-z0-9_+]*)$")
            message(FATAL_ERROR "tlc_test(${name}): SPLIT is ${T_SPLIT}, not <property>=<part>+<part>")
        endif()
        set(split_property ${CMAKE_MATCH_1})
        string(REPLACE "+" ";" split_parts "${CMAKE_MATCH_2}")
        set(args ${ARGN})
        list(FIND args SPLIT at)
        math(EXPR value_at "${at} + 1")
        list(REMOVE_AT args ${at} ${value_at})
        tlc_test(${name}_rest ${module} ${cfg} ${expect} ${args} SPLITMODE rest:${split_property})
        set(i 0)
        foreach(split_part IN LISTS split_parts)
            math(EXPR i "${i} + 1")
            tlc_test(${name}_temporal_${i} ${module} ${cfg} ${expect} ${args}
                SPLITMODE as:${split_property}=${split_part})
        endforeach()
        return()
    endif()
    set(models_dir "${PROJECT_SOURCE_DIR}/models")
    set(tlc_script "${PROJECT_SOURCE_DIR}/tests/model/tlc.cmake")
    if(NOT T_WORKERS)
        set(T_WORKERS 2)
    endif()
    set(label model)
    set(cap 60)
    if(T_NIGHTLY)
        set(label model_nightly)
        set(cap 300)
    endif()
    # SHARD: the instance is the only test of its CI model shard, so the shard's 2 minutes are its budget. 120 s, less
    # the shard's setup before the tests step (checkout, Java and jar check, configure: p90 14 s, worst 29 s over 50
    # runs of each such shard, 2026-11-15) and the steps after it (2-3 s), leaves 88 s; 80 s keeps a margin, so TLC's
    # own cap (which says how far it got) fires before the job is cancelled. A cap is the run's limit, not its time.
    if(T_SHARD)
        set(cap 80)
    endif()
    if(T_CAP)
        set(cap ${T_CAP})
    endif()
    math(EXPR ctest_timeout "${cap} + 30")
    set(defs -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tlc/${name} -DMODELS=${models_dir}
             -DMODULE=${module} -DCFG=${cfg} -DEXPECT=${expect} -DWORKERS=${T_WORKERS} -DCAP=${cap})
    if(T_PROPERTY)
        list(APPEND defs -DPROPERTY=${T_PROPERTY})
    endif()
    if(T_SPLITMODE)
        list(APPEND defs -DSPLIT=${T_SPLITMODE})
    endif()
    set(driver_dist "${PROJECT_SOURCE_DIR}/drivers/claude/dist/test")
    if(T_DRIVERWALK)
        list(APPEND defs -DNODE=${NEXUS_NODE} -DDRIVERWALK=${driver_dist}/walk.js -DSEED=${T_SEED} -DSTEPS=${T_STEPS})
    elseif(T_DRIVERTESTS)
        list(APPEND defs -DNODE=${NEXUS_NODE} -DDRIVERTESTS=${driver_dist}/unit/driver.test.js)
    elseif(T_TSWALK)
        list(APPEND defs -DTSWALK=${PROJECT_SOURCE_DIR}/src/tailscale/lifecycle -DGO=${NEXUS_GO_LOCAL} -DSEED=${T_SEED}
             -DSTEPS=${T_STEPS})
    elseif(T_SIGNWALK)
        list(APPEND defs -DSIGNWALK=$<TARGET_FILE:signin_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS})
    elseif(T_RECONNECTWALK)
        list(APPEND defs -DRECONNECTWALK=$<TARGET_FILE:reconnect_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS})
    elseif(T_SECWALK)
        list(APPEND defs -DSECWALK=$<TARGET_FILE:secrets_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS} -DFORMAT=${T_FORMAT})
    elseif(T_ACCWALK)
        list(APPEND defs -DACCWALK=$<TARGET_FILE:accounts_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS})
    elseif(T_SUPWALK)
        list(APPEND defs -DSUPWALK=$<TARGET_FILE:supervisor_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS})
    elseif(T_STRIPWALK)
        list(APPEND defs -DSTRIPWALK=$<TARGET_FILE:strip_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS}
             -DFRIENDS=${T_FRIENDS})
    elseif(T_GLIDEWALK)
        list(APPEND defs -DGLIDEWALK=$<TARGET_FILE:glide_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS})
    elseif(T_LINKWALK)
        list(APPEND defs -DLINKWALK=$<TARGET_FILE:link_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS} -DWINDOWS=${T_WINDOWS})
    elseif(DEFINED T_SEED AND T_OVERLAY)
        list(APPEND defs -DOVERLAYWALK=$<TARGET_FILE:overlay_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS})
    elseif(DEFINED T_SEED AND T_LABEL)
        list(APPEND defs -DWALK=$<TARGET_FILE:label_walk> -DDATA=TabLabelTraceData -DSEED=${T_SEED} -DSTEPS=${T_STEPS}
             -DFRIENDS=${T_FRIENDS})
    elseif(DEFINED T_SEED)
        list(APPEND defs -DWALK=$<TARGET_FILE:trace_walk> -DSEED=${T_SEED} -DSTEPS=${T_STEPS} -DFRIENDS=${T_FRIENDS})
    endif()
    if(T_FAULTY)
        list(APPEND defs -DFAULTY=${T_FAULTY})
    endif()
    if(T_NOSPRINT)
        list(APPEND defs -DNOSPRINT=ON)
    endif()
    if(T_NODEADLOCK)
        list(APPEND defs -DNOCHECK_DEADLOCK=ON)
    endif()
    if(T_CHURN)
        list(APPEND defs -DCHURN=ON)
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
tlc_test(tlc_witness_take_newest          WTakeNewest         WTakeNewest.cfg         action    PROPERTY TakeExact)
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

# The friends from the server's list (sync/client.h applyFriends): Window.tla with add_friend and remove_friend, behind
# the sprint tab with one friend (39,465 states, 9 s on four workers in a login session), and from an empty strip,
# no sprint tab and no friend (1,930 states); and their reversed witnesses.
tlc_test(tlc_spec_friends       MCWindow MCWindowFriends.cfg      pass WORKERS 8 SHARD)
tlc_test(tlc_spec_friends_empty MCWindow MCWindowFriendsEmpty.cfg pass)
tlc_test(tlc_witness_add_friend_twice     WAddFriendTwice     WAddFriendTwice.cfg     invariant PROPERTY NamesOnce)
tlc_test(tlc_witness_add_friend_among_terminals WAddFriendAmongTerminals WAddFriendAmongTerminals.cfg invariant
         PROPERTY ZonesOrdered)
tlc_test(tlc_witness_add_friend_selects   WAddFriendSelects   WAddFriendSelects.cfg   action    PROPERTY AddFriendExact)
tlc_test(tlc_witness_remove_friend_active_wrong WRemoveFriendActiveWrong WRemoveFriendActiveWrong.cfg action
         PROPERTY RemoveActiveRule)
tlc_test(tlc_witness_remove_friend_keeps_history WRemoveFriendKeepsHistory WRemoveFriendKeepsHistory.cfg invariant
         PROPERTY HistoryOpen)
tlc_test(tlc_witness_close_leaves_history_empty WCloseLeavesHistoryEmpty WCloseLeavesHistoryEmpty.cfg invariant
         PROPERTY HistoryEndsActive)

# Trace binding: the real model's random walks, each a behaviour of Window.tla. 1000 steps take every walk's queues past
# four prompts (trace_walk prints the longest).
foreach(seed 1 2 3 4 5)
    tlc_test(tlc_trace_seed_${seed} WindowTrace WindowTrace.cfg pass SEED ${seed} STEPS 1000 FRIENDS 3)
endforeach()
tlc_test(tlc_trace_two_tabs WindowTrace WindowTrace.cfg pass SEED 6 STEPS 1000 FRIENDS 2)
tlc_test(tlc_trace_one_tab  WindowTrace WindowTrace.cfg pass SEED 8 STEPS 1000 FRIENDS 1)
tlc_test(tlc_trace_no_tabs  WindowTrace WindowTrace.cfg pass SEED 7 STEPS 100 FRIENDS 0)
# Every walk above starts with the sprint tab, as the window does; this one is the model
# without it.
tlc_test(tlc_trace_no_sprint WindowTrace WindowTrace.cfg pass SEED 9 STEPS 1000 FRIENDS 3 NOSPRINT)
# The friends coming and going as the server's list does: from the start's three, and from an empty strip, with
# no sprint tab and no friend.
tlc_test(tlc_trace_friends_churn WindowTrace WindowTrace.cfg pass SEED 10 STEPS 1000 FRIENDS 3 CHURN)
tlc_test(tlc_trace_friends_fresh WindowTrace WindowTrace.cfg pass SEED 11 STEPS 1000 FRIENDS 0 NOSPRINT CHURN)

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
# And with remove_friend leaving the removed tab's id in the selection history.
tlc_test(tlc_trace_faulty_remove_friend_rejected WindowTrace WindowTrace.cfg invariant PROPERTY TraceMatches
         SEED 1 STEPS 300 FRIENDS 3 CHURN FAULTY remove-friend)

# The friend tabs from the server's state (src/sync/friend_strip.h): FriendStrip.tla on two friends (three nightly), its
# reversed witnesses (one a window writing its kept drafts over the file other windows share), and the code's random
# walks checked against it, with two faulty walks that must be rejected.
tlc_test(tlc_spec_strip MCFriendStrip MCFriendStrip.cfg pass WORKERS 4)
tlc_test(tlc_spec_strip_three MCFriendStrip MCFriendStrip3.cfg pass WORKERS 8 NIGHTLY)
# And with another window of the server sharing the kept drafts' file (150,016 states, about 22 s on 4 workers).
tlc_test(tlc_spec_strip_shared MCFriendStrip MCFriendStripShared.cfg pass WORKERS 8 SHARD)
tlc_test(tlc_witness_strip_draft_goes   WStripDraftGoes         WStripDraftGoes.cfg         invariant PROPERTY DraftsKept)
tlc_test(tlc_witness_strip_bad_name     WStripBadName           WStripBadName.cfg           invariant PROPERTY NamesValid)
tlc_test(tlc_witness_strip_never_removes WStripNeverRemoves     WStripNeverRemoves.cfg      invariant PROPERTY StripExact)
tlc_test(tlc_witness_strip_kept_stays   WStripKeptStays         WStripKeptStays.cfg         invariant PROPERTY KeptOnlyAbsent)
tlc_test(tlc_witness_strip_reorders     WStripReorders          WStripReorders.cfg          action PROPERTY SurvivorsKeepOrder)
tlc_test(tlc_witness_strip_saved_order  WStripIgnoresSavedOrder WStripIgnoresSavedOrder.cfg action PROPERTY NewInSavedOrder)
tlc_test(tlc_witness_strip_no_restore   WStripNoRestore         WStripNoRestore.cfg         action PROPERTY RestoreSaved)
tlc_test(tlc_witness_strip_person_loses WStripPersonLoses       WStripPersonLoses.cfg       action PROPERTY PersonWins)
tlc_test(tlc_witness_strip_blind_rewrite WStripBlindRewrite      WStripBlindRewrite.cfg      invariant PROPERTY FileKeepsOthers)
foreach(seed 1 2 3)
    tlc_test(tlc_strip_trace_seed_${seed} FriendStripTrace FriendStripTrace.cfg pass STRIPWALK SEED ${seed} STEPS 400
             FRIENDS 2)
endforeach()
tlc_test(tlc_strip_trace_three FriendStripTrace FriendStripTrace.cfg pass STRIPWALK SEED 4 STEPS 400 FRIENDS 3)
tlc_test(tlc_strip_trace_faulty_forget_rejected FriendStripTrace FriendStripTrace.cfg invariant PROPERTY TraceMatches
         STRIPWALK SEED 1 STEPS 400 FRIENDS 2 FAULTY forget)
tlc_test(tlc_strip_trace_faulty_wait_rejected FriendStripTrace FriendStripTrace.cfg invariant PROPERTY TraceMatches
         STRIPWALK SEED 1 STEPS 400 FRIENDS 2 FAULTY wait)

# A terminal tab's label across ssh sessions (src/remote.h TabLabel): TabLabel.tla, its reversed witnesses, and the code's
# random walks checked against it, with a faulty walk that must be rejected.
tlc_test(tlc_label_spec TabLabel TabLabel.cfg pass)
tlc_test(tlc_witness_label_no_mask         WLabelNoMask        WLabelNoMask.cfg        action PROPERTY EndRestores)
tlc_test(tlc_witness_label_mask_sticks     WLabelMaskSticks    WLabelMaskSticks.cfg    action PROPERTY MaskEnds)
tlc_test(tlc_witness_label_before_at_start WLabelBeforeAtStart WLabelBeforeAtStart.cfg action PROPERTY BeforeIsOwn)
tlc_test(tlc_witness_label_no_tail         WLabelNoTail        WLabelNoTail.cfg        action PROPERTY TailRestores)
if(NEXUS_WINDOW_LOGIC)
    foreach(seed 1 2 3)
        tlc_test(tlc_label_trace_seed_${seed} TabLabelTrace TabLabelTrace.cfg pass LABEL SEED ${seed} STEPS 1000 FRIENDS 2)
    endforeach()
    tlc_test(tlc_label_trace_three_remotes TabLabelTrace TabLabelTrace.cfg pass LABEL SEED 4 STEPS 1000 FRIENDS 3)
    tlc_test(tlc_label_trace_faulty_before_rejected TabLabelTrace TabLabelTrace.cfg invariant PROPERTY TraceMatches
             LABEL SEED 1 STEPS 1000 FRIENDS 2 FAULTY before)
endif()

# The board's scroller (src/scroll_glide.h): Glide.tla on its small instance, every property.
tlc_test(tlc_glide_spec MCGlide MCGlide.cfg pass)
# Its reversed witnesses.
tlc_test(tlc_glide_witness_jump_back     WGlideJumpBack      WGlideJumpBack.cfg      action    PROPERTY NeverBack)
tlc_test(tlc_glide_witness_clamp_top     WGlideClampTop      WGlideClampTop.cfg      invariant PROPERTY InRange)
tlc_test(tlc_glide_witness_pad_keeps     WGlidePadKeeps      WGlidePadKeeps.cfg      action    PROPERTY PadStops)
tlc_test(tlc_glide_witness_lost_key_up   WGlideLostKeyUp     WGlideLostKeyUp.cfg     liveness)
tlc_test(tlc_glide_witness_spring_inside WGlideSpringInside  WGlideSpringInside.cfg  invariant PROPERTY SpringAtEnd)
tlc_test(tlc_glide_witness_spring_forever WGlideSpringForever WGlideSpringForever.cfg liveness)
# Its code traces, each a behaviour of Glide.tla; and a scroller that loses the arrow's release, rejected (no step of
# the spec reaches its next state: a deadlock).
foreach(seed 1 2 3 4 5)
    tlc_test(tlc_glide_trace_seed_${seed} GlideTrace GlideTrace.cfg pass SEED ${seed} STEPS 1000 GLIDEWALK)
endforeach()
tlc_test(tlc_glide_trace_faulty_keyup_rejected GlideTrace GlideTrace.cfg deadlock SEED 1 STEPS 1000 GLIDEWALK
         FAULTY keyup)

# The overlay (Overlay.tla): the whole state space, a witness per property, code traces, and faulty traces rejected.
tlc_test(tlc_overlay_spec Overlay Overlay.cfg pass)
tlc_test(tlc_overlay_witness_act_name          WOverlayActName          WOverlayActName.cfg          invariant
         PROPERTY TypeOK)
tlc_test(tlc_overlay_witness_escape_keeps_keyed WOverlayEscapeKeepsKeyed WOverlayEscapeKeepsKeyed.cfg invariant
         PROPERTY KeyedOnlyVisible)
tlc_test(tlc_overlay_witness_show_unchecked    WOverlayShowUnchecked    WOverlayShowUnchecked.cfg    invariant
         PROPERTY UnkeyedIsChecked)
tlc_test(tlc_overlay_witness_hotkey_hides_unkeyed WOverlayHotkeyHidesUnkeyed WOverlayHotkeyHidesUnkeyed.cfg action
         PROPERTY HotkeyShows)
tlc_test(tlc_overlay_witness_hotkey_only_shows WOverlayHotkeyOnlyShows  WOverlayHotkeyOnlyShows.cfg  action
         PROPERTY HotkeyHides)
tlc_test(tlc_overlay_witness_escape_ignored    WOverlayEscapeIgnored    WOverlayEscapeIgnored.cfg    action
         PROPERTY EscapeHides)
tlc_test(tlc_overlay_witness_hide_while_active WOverlayHideWhileActive  WOverlayHideWhileActive.cfg  action
         PROPERTY NoHideWhileActive)
tlc_test(tlc_overlay_witness_hide_before_key   WOverlayHideBeforeKey    WOverlayHideBeforeKey.cfg    action
         PROPERTY NoHideBeforeKey)
tlc_test(tlc_overlay_witness_loss_kept         WOverlayLossKept         WOverlayLossKept.cfg         action
         PROPERTY LossHides)
tlc_test(tlc_overlay_witness_refused_kept      WOverlayRefusedKept      WOverlayRefusedKept.cfg      action
         PROPERTY RefusedShowHides)
tlc_test(tlc_overlay_witness_stranded          WOverlayStranded         WOverlayStranded.cfg         liveness)
tlc_test(tlc_overlay_witness_held_after_open   WOverlayHeldAfterOpen    WOverlayHeldAfterOpen.cfg    action
         PROPERTY HotkeyShows)
foreach(seed 1 2 3)
    tlc_test(tlc_overlay_trace_seed_${seed} OverlayTrace OverlayTrace.cfg pass OVERLAY SEED ${seed} STEPS 1000)
endforeach()
tlc_test(tlc_overlay_trace_faulty_hide_unkeyed_rejected OverlayTrace OverlayTrace.cfg invariant PROPERTY TraceMatches
         OVERLAY SEED 1 STEPS 1000 FAULTY hide-unkeyed)
tlc_test(tlc_overlay_trace_faulty_hotkey_hides_rejected OverlayTrace OverlayTrace.cfg invariant PROPERTY TraceMatches
         OVERLAY SEED 1 STEPS 1000 FAULTY hotkey-hides)

# The secret dialog (docs/secrets.md): Secrets.tla on four small instances (MCSecretsBad: a paste not of a token's
# shape and a failing disk), every property, and a witness per
# property, per commit (each a second or two); its four larger instances nightly (MCSecretsTwo.cfg took 7 s of the
# model job's 2 minutes; MCSecretsBadTwo.cfg, two requests, 57 s, past the per-commit cap). Bound to the code by trace checks: tests/sync's secrets_walk, tlc_secrets_trace_*.
tlc_test(tlc_secrets_spec      MCSecrets MCSecrets.cfg      pass)
tlc_test(tlc_secrets_spec_one  MCSecrets MCSecretsOne.cfg   pass)
tlc_test(tlc_secrets_spec_old  MCSecrets MCSecretsOld.cfg   pass)
tlc_test(tlc_secrets_spec_two  MCSecrets MCSecretsTwo.cfg   pass WORKERS 4 NIGHTLY)
tlc_test(tlc_secrets_spec_wide  MCSecrets MCSecretsWide.cfg  pass WORKERS 8 NIGHTLY CAP 600)
tlc_test(tlc_secrets_spec_mixed MCSecrets MCSecretsMixed.cfg pass WORKERS 8 NIGHTLY CAP 600)
tlc_test(tlc_secrets_witness_reply_carries_value WSecReplyCarriesValue WSecReplyCarriesValue.cfg invariant
         PROPERTY NoSecretToAsker)
tlc_test(tlc_secrets_witness_transcript_value WSecTranscriptValue WSecTranscriptValue.cfg invariant
         PROPERTY NoSecretInTranscript)
tlc_test(tlc_secrets_witness_log_value      WSecLogValue      WSecLogValue.cfg      invariant PROPERTY NoSecretInLog)
tlc_test(tlc_secrets_witness_relay_to_windows WSecRelayToWindows WSecRelayToWindows.cfg invariant
         PROPERTY NoSecretToWindows)
tlc_test(tlc_secrets_witness_store_twice    WSecStoreTwice    WSecStoreTwice.cfg    invariant PROPERTY AtMostOneStored)
tlc_test(tlc_secrets_witness_late_stored    WSecLateStored    WSecLateStored.cfg    action    PROPERTY ClosedStaysClosed)
tlc_test(tlc_secrets_witness_ids_reused     WSecIdsReused     WSecIdsReused.cfg     action    PROPERTY ClosedStaysClosed)
tlc_test(tlc_secrets_witness_two_open       WSecTwoOpen       WSecTwoOpen.cfg       invariant PROPERTY OneOpenPerAccount)
# Two requests and a failing disk, per commit: an ask beside a held value must be refused busy (541 states).
tlc_test(tlc_secrets_witness_busy_ignores_held WSecBusyIgnoresHeld WSecBusyIgnoresHeld.cfg invariant
         PROPERTY OneOpenPerAccount)
tlc_test(tlc_secrets_witness_reply_twice    WSecReplyTwice    WSecReplyTwice.cfg    invariant PROPERTY AnsweredOnce)
tlc_test(tlc_secrets_witness_no_client_opens WSecNoClientOpens WSecNoClientOpens.cfg action   PROPERTY AskNeedsTaker)
tlc_test(tlc_secrets_witness_no_timeout     WSecNoTimeout     WSecNoTimeout.cfg     liveness)
tlc_test(tlc_secrets_witness_dialog_stays   WSecDialogStays   WSecDialogStays.cfg   liveness)
tlc_test(tlc_secrets_spec_bad  MCSecrets MCSecretsBad.cfg   pass)
tlc_test(tlc_secrets_spec_bad_two MCSecrets MCSecretsBadTwo.cfg pass WORKERS 4 NIGHTLY CAP 600)
tlc_test(tlc_secrets_witness_drop_on_failure WSecDropOnFailure WSecDropOnFailure.cfg invariant
         PROPERTY ReceivedNeverDropped)
tlc_test(tlc_secrets_witness_late_after_deadline WSecLateAfterDeadline WSecLateAfterDeadline.cfg invariant
         PROPERTY ReceivedNeverDropped)
tlc_test(tlc_secrets_witness_stored_before_durable WSecStoredBeforeDurable WSecStoredBeforeDurable.cfg invariant
         PROPERTY StoredOnlyDurable)
tlc_test(tlc_secrets_witness_malformed_stored WSecMalformedStored WSecMalformedStored.cfg invariant
         PROPERTY NoMalformedStored)
tlc_test(tlc_secrets_witness_held_never_retried WSecHeldNeverRetried WSecHeldNeverRetried.cfg liveness)

# The Tailscale shim's node lifecycle (src/tailscale/lifecycle, models/Tailscale.tla): the spec on its small instance,
# every property (its environment comes to rest once every node is closed, hence NODEADLOCK); each reversed witness
# rejected; the code's walks checked against the spec over several seeds; and each of the walk's faults rejected.
tlc_test(tlc_tailscale_spec Tailscale MCTailscale.cfg pass WORKERS 8 SHARD NODEADLOCK)
tlc_test(tlc_tailscale_spec_keys Tailscale MCTailscaleKeys.cfg pass WORKERS 8 SHARD NODEADLOCK)
tlc_test(tlc_tailscale_spec_certs Tailscale MCTailscaleCerts.cfg pass WORKERS 8 SHARD NODEADLOCK)
tlc_test(tlc_tailscale_witness_ready_without_peers Tailscale witnesses/WTsReadyWithoutPeers.cfg invariant NODEADLOCK
         PROPERTY ReadyHasPeers)
tlc_test(tlc_tailscale_witness_url_kept Tailscale witnesses/WTsURLKept.cfg invariant NODEADLOCK
         PROPERTY URLOnlyInNeedsLogin)
tlc_test(tlc_tailscale_witness_closed_runs Tailscale witnesses/WTsClosedRuns.cfg invariant NODEADLOCK
         PROPERTY ClosedNeverRunning)
tlc_test(tlc_tailscale_witness_table_ignores Tailscale witnesses/WTsTableIgnores.cfg invariant NODEADLOCK
         PROPERTY OneNodePerDir)
tlc_test(tlc_tailscale_witness_no_relogin Tailscale witnesses/WTsNoRelogin.cfg liveness NODEADLOCK)
tlc_test(tlc_tailscale_witness_poll_ignored Tailscale witnesses/WTsPollIgnored.cfg liveness NODEADLOCK)
tlc_test(tlc_tailscale_witness_cert_ignored Tailscale witnesses/WTsCertIgnored.cfg invariant NODEADLOCK
         PROPERTY CertNeverReady)
# Orders recorded from the real shim against the test control server: each must be a behaviour of the model's Next,
# its world included (TailscaleOrders.tla: OrderNotReached violated means the model produces the whole order).
foreach(order restart authkey cert starting)
    tlc_test(tlc_tailscale_order_${order} TailscaleOrders TailscaleOrders_${order}.cfg invariant NODEADLOCK
             PROPERTY OrderNotReached)
endforeach()
foreach(seed 1 2 3 4 5)
    tlc_test(tlc_tailscale_trace_seed_${seed} TailscaleTrace TailscaleTrace.cfg pass SEED ${seed} STEPS 1000 TSWALK)
endforeach()
foreach(fault ready url close table down)
    tlc_test(tlc_tailscale_trace_faulty_${fault}_rejected TailscaleTrace TailscaleTrace.cfg deadlock SEED 1 STEPS 1000
             TSWALK FAULTY ${fault})
endforeach()

# In-app sign-in (docs/secrets.md "Sign-in"): SignIn.tla on three small instances, distinct states measured on a
# bench with four workers (MCSignInOne.cfg, one window on another machine, every property: 2,321 states, 1 s;
# MCSignInTwo.cfg, one window on each machine, its safety: 250,670 states, 4 s, and its liveness nightly as
# MCSignInTwoLive.cfg, 45 s; MCSignInGate.cfg, one window on another machine with two flows, its safety: 327,571
# states, 5 s), and its reversed witnesses, each rejected by the rule it breaks (WSignInStrayHostile.cfg passes: a gate
# that sends anything up is still met by the server's own check). MCSignIn.cfg (two flows, a restart, a lost login) is
# a bench run, not a test: an earlier form of the model was past 14 million states in 30 minutes on four workers,
# unfinished. Bound to the code by trace checks: tests/sync's signin_walk, tlc_signin_trace_*.
tlc_test(tlc_signin_spec_one MCSignIn MCSignInOne.cfg pass)
tlc_test(tlc_signin_spec_two MCSignIn MCSignInTwo.cfg pass WORKERS 8 SHARD)
tlc_test(tlc_signin_spec_two_live MCSignIn MCSignInTwoLive.cfg pass WORKERS 8 NIGHTLY)
tlc_test(tlc_signin_spec_gate MCSignIn MCSignInGate.cfg pass WORKERS 8 SHARD)
tlc_test(tlc_signin_witness_gate_taken WSignInGateTaken WSignInGateTaken.cfg invariant PROPERTY GateForEveryPage)
tlc_test(tlc_signin_witness_late_dropped WSignInLateDropped WSignInLateDropped.cfg invariant PROPERTY EveryPrintTaken)
tlc_test(tlc_signin_witness_claim_starts WSignInClaimStarts WSignInClaimStarts.cfg invariant
         PROPERTY OneProcessPerAccount)
tlc_test(tlc_signin_witness_stray_forwards WSignInStrayForwards WSignInStrayForwards.cfg invariant
         PROPERTY GateForwardsOnlyMatching)
tlc_test(tlc_signin_witness_stray_hostile WSignInStrayForwards WSignInStrayHostile.cfg pass WORKERS 8)
tlc_test(tlc_signin_witness_replay_any WSignInStrayForwards WSignInReplayAny.cfg invariant
         PROPERTY CallbackOnlyMatching)
tlc_test(tlc_signin_witness_drop_token WSignInDropToken WSignInDropToken.cfg invariant PROPERTY TokenNeverDropped)
tlc_test(tlc_signin_witness_ask_kept WSignInAskKept WSignInAskKept.cfg invariant PROPERTY NoAskWhenSignedIn)
tlc_test(tlc_signin_witness_token_in_log WSignInTokenInLog WSignInTokenInLog.cfg invariant PROPERTY NoTokenInLog)
tlc_test(tlc_signin_witness_no_timeout WSignInNoTimeout WSignInNoTimeout.cfg liveness)
tlc_test(tlc_signin_witness_need_twice WSignInNeedTwice WSignInNeedTwice.cfg invariant PROPERTY OneFlowPerAccount)
tlc_test(tlc_signin_witness_expired_keeps WSignInExpiredKeeps WSignInExpiredKeeps.cfg invariant
         PROPERTY ProcessHasItsFlow)
