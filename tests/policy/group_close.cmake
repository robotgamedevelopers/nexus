file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}/rules" "${ROOT}/next")
file(COPY "${RULES}/client" "${RULES}/client.star" DESTINATION "${ROOT}/rules")
file(WRITE "${ROOT}/rules/config.json" [=[
{
  "tab_groups": [
    {"id":"dashboards","types":["sprint"],"closable":true},
    {"id":"ais","types":["ai"],"closable":true},
    {"id":"terminals","types":["terminal"],"closable":false},
    {"id":"custom","types":["pinned"],"closable":true}
  ],
  "tab_types": [{"id":"pinned","title":"Pinned","group":"custom","open":"Command+P","closable":false}]
}
]=])
execute_process(COMMAND "${TOOL}" apply client "${ROOT}/rules" "${ROOT}/next" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CHECK}" --group-close "${ROOT}/live.json" "${ROOT}/next/policy.bundle.json"
    COMMAND_ERROR_IS_FATAL ANY)
