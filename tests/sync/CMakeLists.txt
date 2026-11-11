# Tests of the states mosh's transport keeps in step (src/sync), with three labels:
#   unit           sync_test: the states' diffs on random pairs, the properties mosh's Transport relies on (through a
#                  test-only copy of its bookkeeping, fake_mosh.h), the server's rules and the window's mirror; logic
#                  only
#   model          TLC: Link.tla, its reversed witnesses, and the code's walks (link_walk) checked against it. Needs
#                  TLA2TOOLS_JAR and java.
#   model_nightly  TLC: Link.tla on its largest instances

set(warnings -Wall -Wextra -Wpedantic -Werror)

add_executable(sync_test sync_test.cpp)
target_link_libraries(sync_test PRIVATE nexus_sync)
target_compile_options(sync_test PRIVATE ${warnings})

add_executable(link_walk link_walk.cpp)
target_link_libraries(link_walk PRIVATE nexus_sync)
target_compile_options(link_walk PRIVATE ${warnings})

add_dependencies(model_checks sync_test link_walk)

add_test(NAME sync_unit COMMAND sync_test)
set_tests_properties(sync_unit PROPERTIES LABELS unit TIMEOUT 30)

# TLC (tlc_test is tests/model's): Link.tla on four instances, all properties (states and times on four workers in a
# login session on the studio). MCLink.cfg is two windows sharing a tab, one action each of any kind, with an open
# entry and a restart (29,848 states, 5 s); MCLinkOne.cfg one window over two tabs with longer ranges (12,373 states,
# 2 s); MCLinkGrow.cfg one window, one tab, two actions, an open entry grown twice and a restart (17,420 states, 2 s);
# MCLinkTabs.cfg one window over two tabs, open entries in both (54,244 states, 8 s).
# Nightly: MCLinkStops.cfg, two windows sharing a tab, two actions each of any kind (643,703 states, 1 min 53 s), and
# MCLinkWide.cfg, two windows over two tabs, one action each of any kind, an open entry grown once and a restart
# (132,194 states, 37 s).
tlc_test(tlc_link_two_windows MCLink MCLink.cfg      pass WORKERS 4)
tlc_test(tlc_link_one_window  MCLink MCLinkOne.cfg   pass)
tlc_test(tlc_link_grow        MCLink MCLinkGrow.cfg  pass WORKERS 4)
tlc_test(tlc_link_tabs        MCLink MCLinkTabs.cfg  pass WORKERS 4)
tlc_test(tlc_link_stops       MCLink MCLinkStops.cfg pass WORKERS 8 NIGHTLY)
tlc_test(tlc_link_wide        MCLink MCLinkWide.cfg  pass WORKERS 8 NIGHTLY)

# Reversed witnesses: each broken variant must be rejected by the property named.
tlc_test(tlc_link_witness_apply_again    WLinkApplyAgain     WLinkApplyAgain.cfg     invariant PROPERTY ExactlyOnce)
tlc_test(tlc_link_witness_accept_gap     WLinkAcceptGap      WLinkAcceptGap.cfg      invariant PROPERTY ExactlyOnce)
tlc_test(tlc_link_witness_mirror_again   WLinkMirrorAgain    WLinkMirrorAgain.cfg    invariant PROPERTY ViewMatches)
tlc_test(tlc_link_witness_no_notice      WLinkNoNotice       WLinkNoNotice.cfg       invariant PROPERTY ViewMatches)
tlc_test(tlc_link_witness_queue_again    WLinkQueueAgain     WLinkQueueAgain.cfg     invariant PROPERTY QueueMatches)
tlc_test(tlc_link_witness_tail_misaligned WLinkTailMisaligned WLinkTailMisaligned.cfg invariant PROPERTY ViewMatches)
tlc_test(tlc_link_witness_deliver_short  WLinkDeliverShort   WLinkDeliverShort.cfg   liveness  PROPERTY Converges)
tlc_test(tlc_link_witness_mirror_open    WLinkMirrorOpen     WLinkMirrorOpen.cfg     invariant PROPERTY ViewMatches)
tlc_test(tlc_link_witness_close_keeps_open WLinkCloseKeepsOpen WLinkCloseKeepsOpen.cfg invariant
         PROPERTY EachClosedOnce)
tlc_test(tlc_link_witness_take_newest    WLinkTakeNewest     WLinkTakeNewest.cfg     action    PROPERTY OnlyGrow)
tlc_test(tlc_link_witness_restart_no_cut WLinkRestartNoCut   WLinkRestartNoCut.cfg   invariant PROPERTY LiveSound)
tlc_test(tlc_link_witness_stop_twice     WLinkStopTwice      WLinkStopTwice.cfg      invariant PROPERTY ServerHoldsEach)

# Trace binding: the link code's random walks (link_walk), each a behaviour of Link.tla that ends settled. A walk of
# 400 steps records about 130 and takes TLC about a second; 1000 steps over three windows record 345 and take 8.
foreach(seed 1 2 3 4)
    tlc_test(tlc_link_trace_seed_${seed} LinkTrace LinkTrace.cfg pass LINKWALK SEED ${seed} STEPS 400 WINDOWS 2)
endforeach()
tlc_test(tlc_link_trace_one_window    LinkTrace LinkTrace.cfg pass LINKWALK SEED 5 STEPS 400 WINDOWS 1)
tlc_test(tlc_link_trace_three_windows LinkTrace LinkTrace.cfg pass LINKWALK SEED 6 STEPS 600 WINDOWS 3)

# The binding can fail: a faulty Applier, mirror, tail or close must be rejected.
foreach(fault applier mirror tail close)
    tlc_test(tlc_link_trace_faulty_${fault}_rejected LinkTrace LinkTrace.cfg invariant PROPERTY TraceMatches
             LINKWALK SEED 1 STEPS 400 WINDOWS 2 FAULTY ${fault})
endforeach()
