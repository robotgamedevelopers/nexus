# Tests of the accounts and friends (src/accounts) and the hub's decisions about them (src/sync/hub.h), with three
# labels:
#   unit           accounts_test: the files' layout, every refusal with its line, the version rule, the consistency
#                  rules, the accounts the server picks, and the hub's actions, sets, creates, reloads and rotations;
#                  logic only
#   model          TLC: Accounts.tla, its reversed witnesses, and the code's walks (accounts_walk) checked against it.
#                  Needs TLA2TOOLS_JAR and java.
#   functional     accounts_functional_config_repo: the config repository over real git, in a directory of its own
#   model_nightly  TLC: Accounts.tla on its largest instance

set(warnings ${NEXUS_WARNINGS})

add_executable(accounts_test accounts_test.cpp)
target_link_libraries(accounts_test PRIVATE nexus_sync)
target_compile_options(accounts_test PRIVATE ${warnings})

add_dependencies(model_checks accounts_test)

# The walk TLC checks, on Linux and macOS only (NEXUS_SERVER_PARTS in CMakeLists.txt).
if(NEXUS_SERVER_PARTS)
    add_executable(accounts_walk accounts_walk.cpp)
    target_link_libraries(accounts_walk PRIVATE nexus_sync)
    target_compile_options(accounts_walk PRIVATE ${warnings})
    add_dependencies(model_checks accounts_walk)
endif()

foreach(group layout refusals versions consistency picks hub_actions hub_reload_rotate hub_auto_subscriptions hub_store_fails
              hub_adopt_defaults hub_probe instances hub_instances limits keeps_out hub_friends friends_cap)
    add_test(NAME accounts_unit_${group} COMMAND accounts_test ${group})
    set_tests_properties(accounts_unit_${group} PROPERTIES LABELS unit TIMEOUT 30)
endforeach()

# TLC (tlc_test is tests/model's): Accounts.tla, all properties (states and times on two workers on a Linux bench).
# MCAccounts.cfg is two accounts of two providers, two friends, two sessions, every action, reload and report (162,128
# distinct states since a dropped friend's sessions are kept as they were; 14 s on eight workers on an M-series Mac).
# Nightly: MCAccountsThree.cfg, three accounts (two of one provider), one session (1,640,352
# distinct states, 6 min 41 s on two workers; capped at 15 min on eight).
# Per commit on eight workers: 17-23 s, timed two at a time on a loaded tlc runner host (16 cores); alone on an idle
# one (amd-2, 2026-11-15) 16.2 s / 124 CPU s, against 26.3 s / 106 on four. In a CI shard of its own (SHARD: capped at
# the shard's budget, 80 s).
tlc_test(tlc_accounts       MCAccounts MCAccounts.cfg      pass WORKERS 8 SHARD)
tlc_test(tlc_accounts_policy MCAccounts MCAccountsPolicy.cfg pass WORKERS 2)
tlc_test(tlc_accounts_three MCAccounts MCAccountsThree.cfg pass WORKERS 8 NIGHTLY CAP 900)
# MCAccountsInstances.cfg: one account, two friends, two instance names, two sessions: the instances' rules.
tlc_test(tlc_accounts_instances MCAccounts MCAccountsInstances.cfg pass WORKERS 4)
# MCAccountsLimit.cfg: one friend, two accounts of one provider, two sessions, every limit and billing, and drivers that
# start and end: the limit's rules (docs/accounts.md, "limit") beside every other operation (29,175 distinct states,
# 6 s on four workers on a loaded Mac, 2026-11-15).
tlc_test(tlc_accounts_limit MCAccounts MCAccountsLimit.cfg pass WORKERS 4)

# Reversed witnesses: each broken variant must be rejected by the property named.
tlc_test(tlc_accounts_witness_remove_stays       WAccRemoveStays       WAccRemoveStays.cfg       invariant
         PROPERTY SessionLinked)
tlc_test(tlc_accounts_witness_unlink_silent      WAccUnlinkSilent      WAccUnlinkSilent.cfg      action
         PROPERTY MovedIsTold)
tlc_test(tlc_accounts_witness_unlink_keeps_default WAccUnlinkKeepsDefault WAccUnlinkKeepsDefault.cfg invariant
         PROPERTY DefaultLinked)
tlc_test(tlc_accounts_witness_rotate_any         WAccRotateAny         WAccRotateAny.cfg         action
         PROPERTY ServerPicksSignedIn)
tlc_test(tlc_accounts_witness_set_unlinked       WAccSetUnlinked       WAccSetUnlinked.cfg       invariant
         PROPERTY SessionLinked)
tlc_test(tlc_accounts_witness_cross_keeps_harness WAccCrossKeepsHarness WAccCrossKeepsHarness.cfg invariant
         PROPERTY HarnessMatches)
tlc_test(tlc_accounts_witness_rotate_stays       WAccRotateStays       WAccRotateStays.cfg       action
         PROPERTY RotateMoves)
tlc_test(tlc_accounts_witness_create_copies_foreign WAccCreateCopiesForeign WAccCreateCopiesForeign.cfg invariant
         PROPERTY SessionLinked)
tlc_test(tlc_accounts_witness_remove_keeps_links WAccRemoveKeepsLinks  WAccRemoveKeepsLinks.cfg  invariant
         PROPERTY LinksSound)
tlc_test(tlc_accounts_witness_add_resets         WAccAddResets         WAccAddResets.cfg         action
         PROPERTY RefusedChangesNothing)
tlc_test(tlc_accounts_witness_fallback_needs_sign_in WAccFallbackNeedsSignIn WAccFallbackNeedsSignIn.cfg action
         PROPERTY ServerPicksSignedIn)
tlc_test(tlc_accounts_witness_set_moves_other    WAccSetMovesOther     WAccSetMovesOther.cfg     action
         PROPERTY OnlyItsSession)
tlc_test(tlc_accounts_witness_default_unlinked   WAccDefaultUnlinked   WAccDefaultUnlinked.cfg   invariant
         PROPERTY DefaultLinked)
tlc_test(tlc_accounts_witness_set_ignored        WAccSetIgnored        WAccSetIgnored.cfg        action
         PROPERTY SetExact)
tlc_test(tlc_accounts_witness_reload_unchecked   WAccReloadUnchecked   WAccReloadUnchecked.cfg   invariant
         PROPERTY DefaultLinked)
tlc_test(tlc_accounts_witness_provider_keeps_harness WAccProviderKeepsHarness WAccProviderKeepsHarness.cfg invariant
         PROPERTY HarnessMatches)
tlc_test(tlc_accounts_witness_drop_instance_stays WAccDropInstanceStays WAccDropInstanceStays.cfg invariant
         PROPERTY InstanceHeld)
tlc_test(tlc_accounts_witness_add_instance_leaves_none WAccAddInstanceLeavesNone WAccAddInstanceLeavesNone.cfg
         invariant PROPERTY InstanceHeld)
tlc_test(tlc_accounts_witness_move_instance_silent WAccMoveInstanceSilent WAccMoveInstanceSilent.cfg action
         PROPERTY MovedInstanceIsTold)
tlc_test(tlc_accounts_witness_start_ignores_friend WAccStartIgnoresFriend WAccStartIgnoresFriend.cfg invariant
         PROPERTY NeverWeakerThanFriend)
tlc_test(tlc_accounts_witness_start_ignores_billing WAccStartIgnoresBilling WAccStartIgnoresBilling.cfg invariant
         PROPERTY ApiAlwaysLimited)
tlc_test(tlc_accounts_witness_link_api_unlimited WAccLinkApiUnlimited WAccLinkApiUnlimited.cfg invariant
         PROPERTY LimitSound)
tlc_test(tlc_accounts_witness_add_friend_open    WAccAddFriendOpen     WAccAddFriendOpen.cfg     action
         PROPERTY NewFriendLimited)
tlc_test(tlc_accounts_witness_limit_silent       WAccLimitSilent       WAccLimitSilent.cfg       action
         PROPERTY LimitChangeIsTold)
tlc_test(tlc_accounts_witness_drop_moves_sessions WAccDropMovesSessions WAccDropMovesSessions.cfg action
         PROPERTY OutLeftAlone)

# Trace binding: the hub's random walks over accounts (accounts_walk), each a behaviour of Accounts.tla. Each walk of 400
# steps has the server move 5 to 11 sessions (settles and rotations) and takes TLC about two seconds.
foreach(seed 1 2 3 4)
    tlc_test(tlc_accounts_trace_seed_${seed} AccountsTrace AccountsTrace.cfg pass ACCWALK SEED ${seed} STEPS 400)
endforeach()

# The binding can fail: a hub that leaves a session on an account no longer linked to its friend, on its old harness
# when its account's provider changed, or on an instance its friend no longer has, or a server that starts a limited
# friend's driver open, must be rejected.
tlc_test(tlc_accounts_trace_faulty_stay_rejected AccountsTrace AccountsTrace.cfg invariant
         PROPERTY TraceMatches ACCWALK SEED 1 STEPS 400 FAULTY stay)
tlc_test(tlc_accounts_trace_faulty_harness_rejected AccountsTrace AccountsTrace.cfg invariant
         PROPERTY TraceMatches ACCWALK SEED 6 STEPS 400 FAULTY harness)
tlc_test(tlc_accounts_trace_faulty_instance_rejected AccountsTrace AccountsTrace.cfg invariant
         PROPERTY TraceMatches ACCWALK SEED 3 STEPS 400 FAULTY instance)
tlc_test(tlc_accounts_trace_faulty_limit_rejected AccountsTrace AccountsTrace.cfg invariant
         PROPERTY TraceMatches ACCWALK SEED 1 STEPS 400 FAULTY limit)

# The config repository (src/server/config_repo.h): real git in a directory of the test's own, with HOME there too and
# a bare repository beside it as the remote, by path; no network. POSIX, like nexus-server.
if(NOT WIN32)
    add_executable(config_repo_test config_repo_test.cpp)
    target_link_libraries(config_repo_test PRIVATE nexus_config_repo)
    target_compile_options(config_repo_test PRIVATE ${warnings})
    add_test(NAME accounts_functional_config_repo
             COMMAND config_repo_test ${CMAKE_CURRENT_BINARY_DIR}/config_repo_work)
    set_tests_properties(accounts_functional_config_repo PROPERTIES LABELS functional TIMEOUT 60)
endif()
