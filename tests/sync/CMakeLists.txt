# Tests of the states mosh's transport keeps in step (src/sync), with three labels:
#   unit           sync_test: the states' diffs on random pairs, the properties mosh's Transport relies on (through a
#                  test-only copy of its bookkeeping, fake_mosh.h), the server's rules and the window's mirror; logic
#                  only
#   model          TLC: Link.tla, its reversed witnesses, and the code's walks (link_walk) checked against it. Needs
#                  TLA2TOOLS_JAR and java.
#   model_nightly  TLC: Link.tla on its largest instances

set(warnings ${NEXUS_WARNINGS})

# crafted_tables.cpp is the one source of sync_test that includes schema's generated header, so the rest compiles
# beside it.
add_executable(sync_test sync_test.cpp crafted_tables.cpp)
target_link_libraries(sync_test PRIVATE nexus_sync)
# The state format 1 fixtures, written by a nexus from before formats (fixtures/main_state_fixture.cpp).
target_compile_definitions(sync_test PRIVATE NEXUS_SYNC_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_compile_options(sync_test PRIVATE ${warnings})

# The window's link making its session again (src/sync/redial.h): logic only, so it builds everywhere sync_test does.
add_executable(redial_test redial_test.cpp)
target_link_libraries(redial_test PRIVATE nexus_sync)
target_compile_options(redial_test PRIVATE ${warnings})

add_dependencies(model_checks sync_test redial_test)

# A measurement, not a test: encode and decode times of the sessions diff and the actions in each format
# (wire_bench.cpp says which messages). Run by hand (`cmake --build build --target wire_bench`); it reads the clock, and
# is left out of the default build, so CI never spends its 2 minutes on it.
add_executable(wire_bench EXCLUDE_FROM_ALL wire_bench.cpp)
target_link_libraries(wire_bench PRIVATE nexus_sync)
target_compile_options(wire_bench PRIVATE ${warnings})

# The walks TLC checks, on Linux and macOS only (NEXUS_SERVER_PARTS in CMakeLists.txt).
if(NEXUS_SERVER_PARTS)
    add_executable(link_walk link_walk.cpp)
    target_link_libraries(link_walk PRIVATE nexus_sync)
    target_compile_options(link_walk PRIVATE ${warnings})

    # The secret dialog's walk (src/sync/secrets.h through the hub and Drivers over a fake harness, and the windows'
    # prompts), for SecretsTrace.tla.
    add_executable(secrets_walk secrets_walk.cpp)
    target_link_libraries(secrets_walk PRIVATE nexus_durable nexus_drivers)
    target_include_directories(secrets_walk PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(secrets_walk PRIVATE ${warnings})

    # In-app sign-in's walk (src/sync/signin.h through the hub, and the windows' openers and gates), for
    # SignInTrace.tla.
    add_executable(signin_walk signin_walk.cpp)
    target_link_libraries(signin_walk PRIVATE nexus_sync)
    target_include_directories(signin_walk PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(signin_walk PRIVATE ${warnings})

    add_executable(reconnect_walk reconnect_walk.cpp)
    target_link_libraries(reconnect_walk PRIVATE nexus_sync)
    target_compile_options(reconnect_walk PRIVATE ${warnings})

    add_dependencies(model_checks link_walk secrets_walk signin_walk reconnect_walk)
endif()

add_test(NAME sync_unit COMMAND sync_test)
set_tests_properties(sync_unit PROPERTIES LABELS unit TIMEOUT 30)
add_test(NAME sync_unit_redial COMMAND redial_test)
set_tests_properties(sync_unit_redial PROPERTIES LABELS unit TIMEOUT 30)

# Reconnect.tla (models/Reconnect.tla): the spec with every property on three servers in turn, its reversed witnesses
# (the link that never makes a session again, the one that does only when its server says it ended, a backoff that
# never ends, and a session made from a kept CONNECT line), and the code's walks (reconnect_walk) checked against it,
# with two faulty walks rejected (no step of the spec reaches their next state: a deadlock).
tlc_test(tlc_reconnect_spec Reconnect MCReconnect.cfg pass)
tlc_test(tlc_reconnect_witness_once       WReconnectOnce      WReconnectOnce.cfg      liveness)
tlc_test(tlc_reconnect_witness_only_ended WReconnectOnlyEnded WReconnectOnlyEnded.cfg liveness)
tlc_test(tlc_reconnect_witness_gives_up   WReconnectGivesUp   WReconnectGivesUp.cfg   liveness)
tlc_test(tlc_reconnect_witness_cached     WReconnectCached    WReconnectCached.cfg    action PROPERTY DialsLive)
foreach(seed 1 2 3 4)
    tlc_test(tlc_reconnect_trace_seed_${seed} ReconnectTrace ReconnectTrace.cfg pass RECONNECTWALK SEED ${seed} STEPS 1000)
endforeach()
tlc_test(tlc_reconnect_trace_faulty_cached_rejected ReconnectTrace ReconnectTrace.cfg deadlock
         RECONNECTWALK SEED 1 STEPS 1000 FAULTY cached)
tlc_test(tlc_reconnect_trace_faulty_no_reset_rejected ReconnectTrace ReconnectTrace.cfg deadlock
         RECONNECTWALK SEED 1 STEPS 1000 FAULTY no-reset)

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
# window 12 s, tabs 20 s, ensure 32 s. Alone on an idle 16-thread host (amd-2, 2026-11-15), eight workers against four:
# ensure 16.2 s / 124 CPU s against 28.9 / 116, two windows 11.7 / 88 against 19.4 / 77, so they keep eight. Each has
# a CI shard of its own (SHARD: capped at the shard's budget, 80 s).
tlc_test(tlc_link_two_windows MCLink MCLink.cfg      pass WORKERS 8 SHARD)
tlc_test(tlc_link_one_window  MCLink MCLinkOne.cfg   pass WORKERS 8 SHARD)
tlc_test(tlc_link_grow        MCLink MCLinkGrow.cfg  pass WORKERS 4)
tlc_test(tlc_link_tabs        MCLink MCLinkTabs.cfg  pass WORKERS 8 SHARD)
tlc_test(tlc_link_stops       MCLink MCLinkStops.cfg pass WORKERS 8 NIGHTLY)
tlc_test(tlc_link_wide        MCLink MCLinkWide.cfg  pass WORKERS 8 NIGHTLY)
tlc_test(tlc_link_sessions    MCLink MCLinkSessions.cfg pass WORKERS 4)
tlc_test(tlc_link_ensure      MCLink MCLinkEnsure.cfg pass WORKERS 8 SHARD)
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
# The same walks in format 4, schema's tables: each value rides as table bytes, through the wiping allocator and the
# wiped read region (src/sync/tables_actions.cpp), so that path is bound to the model too. The walks above stay in
# format 3, the hand-written codec, which windows of format 3 still use.
foreach(seed 1 2 3 4)
    tlc_test(tlc_secrets_trace_tables_seed_${seed} SecretsTrace SecretsTrace.cfg pass SECWALK SEED ${seed} STEPS 2000
             FORMAT 4)
endforeach()
foreach(seed 1 2 3 4)
    tlc_test(tlc_secrets_trace_format5_seed_${seed} SecretsTrace SecretsTrace.cfg pass SECWALK SEED ${seed} STEPS 2000
             FORMAT 5)
endforeach()
tlc_test(tlc_secrets_trace_tables_faulty_echo_rejected SecretsTrace SecretsTrace.cfg invariant PROPERTY CodeNoSecretInLog
         SECWALK SEED 1 STEPS 2000 FAULTY echo FORMAT 4)
tlc_test(tlc_secrets_trace_tables_faulty_driver_rejected SecretsTrace SecretsTrace.cfg invariant PROPERTY TraceMatches
         SECWALK SEED 1 STEPS 2000 FAULTY driver FORMAT 4)
tlc_test(tlc_secrets_trace_tables_faulty_driver-echo_rejected SecretsTrace SecretsTrace.cfg invariant
         PROPERTY CodeNoSecretToAsker SECWALK SEED 1 STEPS 2000 FAULTY driver-echo FORMAT 4)
# Seed 9: a walk with late answers in it (two, both played); with the held values in the walk, seed 1 has none.
tlc_test(tlc_secrets_trace_faulty_store-late_rejected SecretsTrace SecretsTrace.cfg invariant PROPERTY TraceMatches
         SECWALK SEED 9 STEPS 2000 FAULTY store-late)
# The echo is injected into the code (SecretDesk::injectEchoFault): the value reaches server.log's bytes, and the
# code-side invariant names it.
tlc_test(tlc_secrets_trace_faulty_echo_rejected SecretsTrace SecretsTrace.cfg invariant PROPERTY CodeNoSecretInLog
         SECWALK SEED 1 STEPS 2000 FAULTY echo)
# A session asker's `secret` line dropped before Drivers::tellSecret never reaches the driver's standard input, and one
# carrying the value on the driver's standard input is caught by CodeNoSecretToAsker.
tlc_test(tlc_secrets_trace_faulty_driver_rejected SecretsTrace SecretsTrace.cfg invariant PROPERTY TraceMatches
         SECWALK SEED 1 STEPS 2000 FAULTY driver)
tlc_test(tlc_secrets_trace_faulty_driver-echo_rejected SecretsTrace SecretsTrace.cfg invariant
         PROPERTY CodeNoSecretToAsker SECWALK SEED 1 STEPS 2000 FAULTY driver-echo)

# Trace binding of in-app sign-in (models/SignInTrace.tla): the code's random walks (signin_walk), each a behaviour of
# SignIn.tla with every safety property, the windows' views never holding a login and no log line any value. The
# faulty walks (a takeover that starts a second process, a callback replayed twice, a gate that takes a stray request)
# must each be rejected by the rule they break, on the code's own record. The claim fault runs on seed 2: seed 1's walk
# takes no takeover since the walk gained the late print and the fallback, so its fault is never played. Seed 22's walk
# prints a login late while the paste the fallback asked for is open (it counts print-while-asked), which must close it.
foreach(seed 1 2 3 4 22)
    tlc_test(tlc_signin_trace_seed_${seed} SignInTrace SignInTrace.cfg pass SIGNWALK SEED ${seed} STEPS 6000 CAP 80)
endforeach()
tlc_test(tlc_signin_trace_faulty_claim_rejected SignInTrace SignInTrace.cfg invariant
         PROPERTY CodeOneProcessPerAccount SIGNWALK SEED 2 STEPS 6000 FAULTY claim CAP 80)
tlc_test(tlc_signin_trace_faulty_callback_rejected SignInTrace SignInTrace.cfg invariant
         PROPERTY CodeOneCallbackPerFlow SIGNWALK SEED 1 STEPS 6000 FAULTY callback CAP 80)
tlc_test(tlc_signin_trace_faulty_stray_rejected SignInTrace SignInTrace.cfg invariant
         PROPERTY CodeGateForwardsOnlyMatching SIGNWALK SEED 1 STEPS 6000 FAULTY stray CAP 80)
