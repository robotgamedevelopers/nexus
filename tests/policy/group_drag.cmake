file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}/rules" "${ROOT}/locked" "${ROOT}/transfer")
file(COPY "${RULES}/client" "${RULES}/client.star" DESTINATION "${ROOT}/rules")
set(config [=[
{
  "tab_groups": [
    {"id":"Tools","types":["terminal","words"]},
    {"id":"Chat","types":["friend","left"]},
    {"id":"Review","types":["sprint","right"],"closable":true}
  ],
  "tab_types": [
    {"id":"left","title":"Left","group":"Chat","open":"Command+L","native":"text_editor"},
    {"id":"right","title":"Right","group":"Review","open":"Command+R","native":"text_editor"},
    {"id":"words","title":"Words","group":"Tools","open":"Command+Y","panels":["text_area"]}
  ]
}
]=])
file(WRITE "${ROOT}/rules/config.json" "${config}")
execute_process(COMMAND "${TOOL}" apply client "${ROOT}/rules" "${ROOT}/locked" COMMAND_ERROR_IS_FATAL ANY)
string(JSON config SET "${config}" tab_groups 0 accepts_from [=[["Review"]]=])
file(WRITE "${ROOT}/rules/config.json" "${config}")
execute_process(COMMAND "${TOOL}" apply client "${ROOT}/rules" "${ROOT}/transfer" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CHECK}" --group-drag "${ROOT}/live.json" "${ROOT}/locked/policy.bundle.json"
    "${ROOT}/transfer/policy.bundle.json" COMMAND_ERROR_IS_FATAL ANY)
