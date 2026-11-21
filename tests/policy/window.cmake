file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}/rules" "${ROOT}/activated")
file(COPY "${RULES}/client" DESTINATION "${ROOT}/rules")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/fixtures/plugins/config.json" "${ROOT}/rules/config.json")
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

# Exercise the fixture's server application with its own config under the test root.
file(COPY "${RULES}/server" DESTINATION "${ROOT}/rules")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/fixtures/plugins/server.star" "${ROOT}/rules/server.star")
file(WRITE "${ROOT}/message.json" [=[{"friend":"word-count","text":"one two three","intent":"send"}]=])
execute_process(COMMAND "${TOOL}" preview server "${ROOT}/rules" app.message INPUT_FILE "${ROOT}/message.json"
    OUTPUT_VARIABLE reply OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(JSON words GET "${reply}" 0 text)
if(NOT words STREQUAL "3 words")
    message(FATAL_ERROR "word-count fixture returned ${reply}")
endif()
