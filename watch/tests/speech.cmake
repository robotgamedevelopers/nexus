# The speech test (functional, macOS): recorded speech through the recognizer the phone uses, then the watch's name
# matcher. Each case is spoken by `say` into an AAC recording like the watch's, transcribed by
# nexus-watch-relay --transcribe (SpeechTranscriber, on device), and matched by watch_match against the AIs.
#
#   cmake -DRELAY=<nexus-watch-relay> -DMATCH=<watch_match> -DWORK=<dir> [-DPLANT=1] -P speech.cmake
#
# PLANT=1 expects the wrong AI for the first case: the test must then fail (watch_speech_planted, WILL_FAIL), so a
# checker that cannot fail is caught.

set(ais "emma,stella,zhi-ya-wen,johnny")
# <what is said> | <expected match line, as a regex>
set(cases
    "Emma, what is the weather like on the moon?|^named emma\\|[Ww]hat is the weather like on the moon\\?$"
    "Hey Stella, read me the build log.|^named stella\\|[Rr]ead me the build log\\.?$"
    "Johnny, ship it.|^named johnny\\|[Ss]hip it\\.?$"
    "What is on the board today?|^none\\|What is on the board today\\?$")
if(PLANT)
    list(GET cases 0 first)
    string(REPLACE "named emma" "named stella" first "${first}")
    list(REMOVE_AT cases 0)
    list(INSERT cases 0 "${first}")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(failed 0)
set(n 0)
foreach(c IN LISTS cases)
    math(EXPR n "${n} + 1")
    string(FIND "${c}" "|" bar)
    string(SUBSTRING "${c}" 0 ${bar} spoken)
    math(EXPR bar "${bar} + 1")
    string(SUBSTRING "${c}" ${bar} -1 expect)
    set(audio "${WORK}/case-${n}.m4a")
    execute_process(COMMAND say -o "${audio}" --file-format=m4af --data-format=aac "${spoken}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "speech: say could not record case ${n} (${rc})")
    endif()
    execute_process(COMMAND "${RELAY}" --transcribe "${audio}" OUTPUT_VARIABLE heard ERROR_VARIABLE why
        RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 90)
    if(NOT rc EQUAL 0)
        message(SEND_ERROR "speech: case ${n} \"${spoken}\": the recognizer failed (${rc}): ${why}")
        set(failed 1)
        continue()
    endif()
    execute_process(COMMAND "${MATCH}" "${ais}" "${heard}" OUTPUT_VARIABLE got RESULT_VARIABLE rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(rc EQUAL 0 AND got MATCHES "${expect}")
        message(STATUS "speech: case ${n} \"${spoken}\" -> \"${heard}\" -> ${got}")
    else()
        message(SEND_ERROR "speech: case ${n} \"${spoken}\" -> \"${heard}\" -> \"${got}\", expected ${expect}")
        set(failed 1)
    endif()
endforeach()

# Silence: the recognizer must say there were no words, never hand back an empty prompt.
set(audio "${WORK}/silence.m4a")
execute_process(COMMAND say -o "${audio}" --file-format=m4af --data-format=aac "[[slnc 1500]]" RESULT_VARIABLE rc)
execute_process(COMMAND "${RELAY}" --transcribe "${audio}" OUTPUT_VARIABLE heard ERROR_VARIABLE why
    RESULT_VARIABLE rc TIMEOUT 90)
if(rc EQUAL 1 AND why MATCHES "no words: there were no words in the recording")
    message(STATUS "speech: silence -> ${why}")
else()
    message(SEND_ERROR "speech: silence gave (${rc}) \"${heard}\" \"${why}\", expected the no-words failure")
    set(failed 1)
endif()
if(failed)
    message(FATAL_ERROR "speech: failed")
endif()
