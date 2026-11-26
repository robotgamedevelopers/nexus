# The bundles `nexus-policy apply <side> policy/rules` writes from this commit are the ones the hosts package, so a
# deploy that applies again from the same commit leaves no warning (tests/policy/stale_bundle.cpp).
file(REMOVE_RECURSE "${ROOT}")
foreach(side IN ITEMS client server)
    file(MAKE_DIRECTORY "${ROOT}/${side}")
    execute_process(COMMAND "${TOOL}" apply ${side} "${RULES}" "${ROOT}/${side}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(COMMAND "${CHECK}" "${ROOT}" COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE_RECURSE "${ROOT}")
