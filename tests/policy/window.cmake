file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}/rules" "${ROOT}/activated" "${ROOT}/notices")
file(COPY "${RULES}/client" DESTINATION "${ROOT}/rules")
file(READ "${CMAKE_CURRENT_LIST_DIR}/fixtures/plugins/config.json" config)
string(JSON config SET "${config}" overlay_opacity 0.35)
string(JSON config SET "${config}" message_card [[{"fill":"#edf2ea","ink":"#192218","text_size":38,"radius":24}]])
string(JSON config SET "${config}" transcript_style [[{"fill":"#102030","ink":"#f0e0d0","notice_ink":"#ffa0a0","status_ink":"#cfdfa0","radius":12,"show_notices":false,"show_tools":false}]])
string(JSON config SET "${config}" account_shortcuts [[{"Command+ArrowUp":"custom"}]])
file(WRITE "${ROOT}/rules/config.json" "${config}")
file(WRITE "${ROOT}/rules/client.star" [=[
load("client/window.star", stock = "handle")
def handle(event, state, config):
    if event == "prompt_key":
        return [{"op": "queue", "text": "edited " + state["text"]}]
    window = stock(event, state, config)
    if state["group"] == "validate-style":
        cases = [
            None, {}, {"ink": "#aAbBcC", "radius": 0}, {"radius": 64},
            "bad", {"fill": False}, {"notice_ink": "#234"}, {"status_ink": "#zzzzzz"},
            {"radius": 65}, {"radius": -1}, {"radius": True}, {"show_notices": 0},
            {"show_tools": "false"}, {"show_tool": False},
        ]
        style = cases[state["active_tab"]]
        if style == None:
            window.pop("transcript_style")
        else:
            window["transcript_style"] = style
        window["overlay_opacity"] = 0.125
    window["shortcuts"].append(["Command+K", "zoom", 2])
    return window
]=])
execute_process(COMMAND "${TOOL}" apply client "${ROOT}/rules" "${ROOT}/activated" COMMAND_ERROR_IS_FATAL ANY)
string(JSON config SET "${config}" transcript_style show_notices true)
file(WRITE "${ROOT}/rules/config.json" "${config}")
execute_process(COMMAND "${TOOL}" apply client "${ROOT}/rules" "${ROOT}/notices" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CHECK}" "${ROOT}/live.json" "${ROOT}/activated/policy.bundle.json"
    "${ROOT}/notices/policy.bundle.json" COMMAND_ERROR_IS_FATAL ANY)

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

# The same server executable resolves a fork-defined command and changes wrap behavior from JSON.
file(COPY_FILE "${RULES}/server.star" "${ROOT}/rules/server.star")
string(JSON config SET "${config}" account_navigation [[{"steps":{"custom":-1},"wrap":false}]])
file(WRITE "${ROOT}/rules/config.json" "${config}")
file(WRITE "${ROOT}/choice.json" [[{"current":"a","linked":["a","b"],"command":"custom"}]])
execute_process(COMMAND "${TOOL}" preview server "${ROOT}/rules" account_command INPUT_FILE "${ROOT}/choice.json"
    OUTPUT_VARIABLE choice OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT choice STREQUAL "\"a\"")
    message(FATAL_ERROR "bounded account command returned ${choice}")
endif()
string(JSON config SET "${config}" account_navigation wrap true)
file(WRITE "${ROOT}/rules/config.json" "${config}")
execute_process(COMMAND "${TOOL}" preview server "${ROOT}/rules" account_command INPUT_FILE "${ROOT}/choice.json"
    OUTPUT_VARIABLE choice OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT choice STREQUAL "\"b\"")
    message(FATAL_ERROR "wrapped account command returned ${choice}")
endif()

file(WRITE "${ROOT}/new-session.json" [[{"friend":"friend","source":null,"defaults":{"account":"","instance":"","model":"","effort":""},"roster":{"linked":["a","b"],"preferred":"b","instances":[]}}]])
foreach(mode preferred first)
    if(mode STREQUAL "first")
        string(JSON config SET "${config}" account_default [["first"]])
        file(WRITE "${ROOT}/rules/config.json" "${config}")
    endif()
    execute_process(COMMAND "${TOOL}" preview server "${ROOT}/rules" app.new_session INPUT_FILE "${ROOT}/new-session.json"
        OUTPUT_VARIABLE reply OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    string(JSON chosen GET "${reply}" account)
    if((mode STREQUAL "preferred" AND NOT chosen STREQUAL "b") OR (mode STREQUAL "first" AND NOT chosen STREQUAL "a"))
        message(FATAL_ERROR "expected ${mode} default, got ${chosen}")
    endif()
endforeach()
