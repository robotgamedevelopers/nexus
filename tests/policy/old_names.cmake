# A fork's policy from before nexus called its AIs "AI" (docs/policy.md, "Names before AI"): a source or config.json
# still naming the old tab kind or directory is refused at apply, naming the file, the line and the new name, and
# nothing is written. The stock rules apply.
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}/source" "${ROOT}/config" "${ROOT}/host")
file(WRITE "${ROOT}/source/client.star" "def handle(event, state, config):\n    if state[\"tab\"] == \"friend\":\n        return {}\n    return None\n")
file(WRITE "${ROOT}/source/config.json" "{}\n")
file(WRITE "${ROOT}/config/client.star" "def handle(event, state, config):\n    return None\n")
file(WRITE "${ROOT}/config/config.json" [=[{"tab_groups": [{"id": "mine", "types": ["friend"]}]}]=])

execute_process(COMMAND "${TOOL}" apply client "${ROOT}/source" "${ROOT}/host" RESULT_VARIABLE status
    ERROR_VARIABLE said OUTPUT_QUIET)
if(status EQUAL 0 OR NOT said MATCHES "client.star:2: \"friend\" is \"ai\"" OR EXISTS "${ROOT}/host/policy.bundle.json")
    message(FATAL_ERROR "an old tab kind in a source was not refused (${status}): ${said}")
endif()
execute_process(COMMAND "${TOOL}" apply client "${ROOT}/config" "${ROOT}/host" RESULT_VARIABLE status
    ERROR_VARIABLE said OUTPUT_QUIET)
if(status EQUAL 0 OR NOT said MATCHES "config.json.tab_groups\\[0\\].types\\[0\\] is \"friend\"; it is \"ai\" now"
        OR EXISTS "${ROOT}/host/policy.bundle.json")
    message(FATAL_ERROR "an old tab kind in config.json was not refused (${status}): ${said}")
endif()
execute_process(COMMAND "${TOOL}" apply client "${RULES}" "${ROOT}/host" COMMAND_ERROR_IS_FATAL ANY OUTPUT_QUIET)
