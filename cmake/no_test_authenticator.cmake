# The shipped nexus has none of the client lock's stand-in authenticator (src/lock_test_authenticator.cpp, CMake option
# NEXUS_TEST_AUTHENTICATOR): run as `cmake -DBINARY=<the app's executable> -P no_test_authenticator.cmake`, after every
# link of the app and as the unit test nexus_no_test_authenticator. It fails, loudly, on a binary that holds the
# stand-in's words, and on a binary it cannot read. (Run on this file itself, it fails: the test
# nexus_no_test_authenticator_detects, which shows that the check can fail.)
if(NOT DEFINED BINARY OR NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "no_test_authenticator: no binary to check (BINARY=${BINARY})")
endif()
file(STRINGS "${BINARY}" found REGEX "TEST BUILD's authenticator|lock-test-answer")
if(found)
    message(FATAL_ERROR "no_test_authenticator: ${BINARY} holds the lock's stand-in authenticator, which unlocks with "
        "no real check; a build that ships must have NEXUS_TEST_AUTHENTICATOR OFF")
endif()
message(STATUS "no_test_authenticator: ${BINARY} has none of the stand-in")
