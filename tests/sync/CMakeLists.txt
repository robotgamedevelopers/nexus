# Tests of the states mosh's transport keeps in step (src/sync), with three labels:
#   unit           sync_test: the states' diffs on random pairs, the properties mosh's Transport relies on (through a
#                  test-only copy of its bookkeeping, fake_mosh.h), the server's rules and the window's mirror; logic
#                  only
#   model          TLC: Link.tla, its reversed witnesses, and the code's walks (link_walk) checked against it. Needs
#                  TLA2TOOLS_JAR and java.
#   model_nightly  TLC: Link.tla on its largest instances

set(warnings ${NEXUS_WARNINGS})

add_executable(sync_test sync_test.cpp)
target_link_libraries(sync_test PRIVATE nexus_sync)
# The state format 1 fixtures, written by a nexus from before formats (fixtures/main_state_fixture.cpp).
target_compile_definitions(sync_test PRIVATE NEXUS_SYNC_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_compile_options(sync_test PRIVATE ${warnings})

add_executable(link_walk link_walk.cpp)
target_link_libraries(link_walk PRIVATE nexus_sync)
target_compile_options(link_walk PRIVATE ${warnings})

# The secret dialog's walk (src/sync/secrets.h through the hub, and the windows' prompts), for SecretsTrace.tla.
add_executable(secrets_walk secrets_walk.cpp)
target_link_libraries(secrets_walk PRIVATE nexus_durable)
target_include_directories(secrets_walk PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_options(secrets_walk PRIVATE ${warnings})

add_dependencies(model_checks sync_test link_walk secrets_walk)

add_test(NAME sync_unit COMMAND sync_test)
set_tests_properties(sync_unit PROPERTIES LABELS unit TIMEOUT 30)

# TLC (tlc_test is tests/model's): Link.tla on six instances, all properties (states and times on four workers in a
# login session on the studio). MCLink.cfg is two windows sharing one friend's session, one send, queue or stop each,
# with an open entry and a restart (37,306 states, 15 s); MCLinkOne.cfg one window over two friends' sessions with
# longer ranges (15,680 states, 6 s); MCLinkGrow.cfg one window, one session, two actions, an open entry grown twice
# and a restart (19,050 states, 4 s); MCLinkTabs.cfg one window over two sessions, open entries in both (58,128
# states, 18 s); MCLinkSessions.cfg one window, friend a's session at the start and none for b, creates and ensures
# among two actions of any kind (6,331 states, 2 s); MCLinkEnsure.cfg two windows ensuring the one friend's session
# from none and sending in what they show (70,191 states, 28 s).
# Nightly: MCLinkStops.cfg, two windows sharing a session, two actions each (736,080 states, 2 min 27 s on eight
# workers); MCLinkWide.cfg, two windows over two sessions, one action each, an open entry grown once and a restart
# (171,304 states, 1 min 15 s); MCLinkSessionsWide.cfg, MCLinkSessions.cfg with three actions (240,308 states, 1 min).
# Per commit on eight workers each, timed two at a time on a loaded tlc runner host (16 cores): two windows 21 s, one
# window 12 s, tabs 20 s, ensure 32 s (62 s on four workers there, so it is capped at 75 s).
tlc_test(tlc_link_two_windows MCLink MCLink.cfg      pass WORKERS 8)
tlc_test(tlc_link_one_window  MCLink MCLinkOne.cfg   pass WORKERS 8)
tlc_test(tlc_link_grow        MCLink MCLinkGrow.cfg  pass WORKERS 4)
tlc_test(tlc_link_tabs        MCLink MCLinkTabs.cfg  pass WORKERS 8)
tlc_test(tlc_link_stops       MCLink MCLinkStops.cfg pass WORKERS 8 NIGHTLY)
tlc_test(tlc_link_wide        MCLink MCLinkWide.cfg  pass WORKERS 8 NIGHTLY)
tlc_test(tlc_link_sessions    MCLink MCLinkSessions.cfg pass WORKERS 4)
tlc_test(tlc_link_ensure      MCLink MCLinkEnsure.cfg pass WORKERS 8 CAP 75)
tlc_test(tlc_link_sessions_wide MCLink MCLinkSessionsWide.cfg pass WORKERS 8 NIGHTLY)

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
tlc_test(tlc_link_witness_id_reuse       WLinkIdReuse        WLinkIdReuse.cfg        invariant PROPERTY IdsNeverReused)
tlc_test(tlc_link_witness_ensure_again   WLinkEnsureAgain    WLinkEnsureAgain.cfg    invariant PROPERTY EnsureMakesOne)
tlc_test(tlc_link_witness_show_newest    WLinkShowNewest     WLinkShowNewest.cfg     invariant PROPERTY ShowsFirst)

# Trace binding: the link code's random walks (link_walk), each a behaviour of Link.tla that ends settled. A walk of
# 400 steps records about 130 and takes TLC about a second; 1000 steps over three windows record 345 and take 8.
foreach(seed 1 2 3 4)
    tlc_test(tlc_link_trace_seed_${seed} LinkTrace LinkTrace.cfg pass LINKWALK SEED ${seed} STEPS 400 WINDOWS 2)
endforeach()
tlc_test(tlc_link_trace_one_window    LinkTrace LinkTrace.cfg pass LINKWALK SEED 5 STEPS 400 WINDOWS 1)
tlc_test(tlc_link_trace_three_windows LinkTrace LinkTrace.cfg pass LINKWALK SEED 6 STEPS 600 WINDOWS 3)

# The binding can fail: a faulty Applier, mirror, tail, close or ensure must be rejected.
foreach(fault applier mirror tail close ensure)
    tlc_test(tlc_link_trace_faulty_${fault}_rejected LinkTrace LinkTrace.cfg invariant PROPERTY TraceMatches
             LINKWALK SEED 1 STEPS 400 WINDOWS 2 FAULTY ${fault})
endforeach()

# Trace binding of the secret dialog (models/SecretsTrace.tla): the code's random walks (secrets_walk), each a
# behaviour of Secrets.tla with every safety property, and each checking that no value's bytes reach a session line,
# the store, a log line, an answer, or a state sent to any window. The faulty walks (a late answer stored anyway, a
# value in the log) must be rejected.
foreach(seed 1 2 3 4)
    tlc_test(tlc_secrets_trace_seed_${seed} SecretsTrace SecretsTrace.cfg pass SECWALK SEED ${seed} STEPS 2000)
endforeach()
tlc_test(tlc_secrets_trace_faulty_store-late_rejected SecretsTrace SecretsTrace.cfg invariant PROPERTY TraceMatches
         SECWALK SEED 1 STEPS 2000 FAULTY store-late)
# The echo is injected into the code (SecretDesk::injectEchoFault): the value reaches server.log's bytes, and the
# code-side invariant names it.
tlc_test(tlc_secrets_trace_faulty_echo_rejected SecretsTrace SecretsTrace.cfg invariant PROPERTY CodeNoSecretInLog
         SECWALK SEED 1 STEPS 2000 FAULTY echo)
