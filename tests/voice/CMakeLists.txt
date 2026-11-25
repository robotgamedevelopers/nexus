# Voice (src/voice): which AI a spoken prompt is for, and one voice turn of a client, with three labels:
#   unit   name_match_unit, voice_turn_unit and voice_host_unit (over a fake Speech): pure logic, no OS, no audio, no clock (the time is an argument)
#   model  TLC: VoiceTurn.tla on two AIs, every reversed witness rejected, the code's random walks checked against
#          the spec over several seeds, and a faulty turn's trace rejected. Needs TLA2TOOLS_JAR and java.
# The OS's speech (src/voice/speech.h) is tested on the iOS simulator, with recorded audio (tests/ios/tests.cmake).

add_executable(name_match_test test_name_match.cpp)
target_link_libraries(name_match_test PRIVATE nexus_voice)
target_compile_options(name_match_test PRIVATE ${NEXUS_WARNINGS})
add_test(NAME name_match_unit COMMAND name_match_test)
set_tests_properties(name_match_unit PROPERTIES LABELS unit TIMEOUT 30)

add_executable(voice_turn_test test_voice_turn.cpp)
target_link_libraries(voice_turn_test PRIVATE nexus_voice)
target_compile_options(voice_turn_test PRIVATE ${NEXUS_WARNINGS})
add_test(NAME voice_turn_unit COMMAND voice_turn_test)
set_tests_properties(voice_turn_unit PROPERTIES LABELS unit TIMEOUT 30)

add_executable(voice_host_test test_voice_host.cpp)
target_link_libraries(voice_host_test PRIVATE nexus_voice)
target_compile_options(voice_host_test PRIVATE ${NEXUS_WARNINGS})
add_test(NAME voice_host_unit COMMAND voice_host_test)
set_tests_properties(voice_host_unit PROPERTIES LABELS unit TIMEOUT 30)

if(NEXUS_SERVER_PARTS)
    # The walk whose traces VoiceTurnTrace.tla checks.
    add_executable(voice_walk voice_walk.cpp)
    target_link_libraries(voice_walk PRIVATE nexus_voice)
    target_compile_options(voice_walk PRIVATE ${NEXUS_WARNINGS})
    add_dependencies(model_checks name_match_test voice_turn_test voice_host_test voice_walk)
endif()

# A client's voice turn (src/voice/voice_turn.h): VoiceTurn.tla on two AIs, every property; its reversed witnesses,
# each rejected by the property it breaks; the code's walks, each a behaviour of the spec; and a turn that keeps an AI
# awaited after its turn ended, rejected.
tlc_test(tlc_voice_spec MCVoiceTurn MCVoiceTurn.cfg pass)
tlc_test(tlc_voice_witness_listen_over_speech WVoiceListenOverSpeech witnesses/WVoiceListenOverSpeech.cfg invariant
         PROPERTY NeverHearsItself)
tlc_test(tlc_voice_witness_speaks_anything WVoiceSpeaksAnything witnesses/WVoiceSpeaksAnything.cfg action
         PROPERTY SpeaksAnswers)
tlc_test(tlc_voice_witness_fail_loses WVoiceFailLoses witnesses/WVoiceFailLoses.cfg action PROPERTY NothingLost)
tlc_test(tlc_voice_witness_waits_forever WVoiceWaitsForever witnesses/WVoiceWaitsForever.cfg liveness)
tlc_test(tlc_voice_witness_text_speaks WVoiceTextSpeaks witnesses/WVoiceTextSpeaks.cfg invariant PROPERTY TextIsSilent)
tlc_test(tlc_voice_witness_denied_listens WVoiceDeniedListens witnesses/WVoiceDeniedListens.cfg invariant
         PROPERTY DeniedNeverListens)
foreach(seed 1 2 3 4)
    tlc_test(tlc_voice_trace_seed_${seed} VoiceTurnTrace VoiceTurnTrace.cfg pass VOICEWALK SEED ${seed} STEPS 600)
endforeach()
tlc_test(tlc_voice_trace_faulty_turn_end_rejected VoiceTurnTrace VoiceTurnTrace.cfg invariant PROPERTY TraceMatches
         VOICEWALK SEED 1 STEPS 600 FAULTY turn-end)

# The iPhone's form as the shipped client policy composes it, read by the window's own reader (src/window_app.h): with
# the desktop's view library here, and the iOS build's in tests/ios/tests.cmake. Pure logic over the packaged Starlark.
if(TARGET nexus_view)
    add_executable(phone_policy_test test_phone_policy.cpp)
    target_link_libraries(phone_policy_test PRIVATE nexus_view)
    target_compile_options(phone_policy_test PRIVATE ${NEXUS_WARNINGS})
    add_test(NAME phone_policy_unit COMMAND phone_policy_test)
    set_tests_properties(phone_policy_unit PROPERTIES LABELS unit TIMEOUT 30)
endif()
