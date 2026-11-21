file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}/rules" "${ROOT}/activated")
file(COPY "${RULES}/client" DESTINATION "${ROOT}/rules")
file(COPY_FILE "${RULES}/../examples/editor.json" "${ROOT}/rules/config.json")
file(WRITE "${ROOT}/rules/client.star" [=[
load("client/window.star", stock = "handle")
def handle(event, state, config):
    if event == "prompt_key":
        return [{"op": "queue", "text": "edited " + state["text"]}]
    window = stock(event, state, config)
    window["shortcuts"].append(["Command+K", "zoom", 2])
    return window
]=])
execute_process(COMMAND "${TOOL}" apply client "${ROOT}/rules" "${ROOT}/activated" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CHECK}" "${ROOT}/live.json" "${ROOT}/activated/policy.bundle.json" COMMAND_ERROR_IS_FATAL ANY)
