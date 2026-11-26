// Unit tests of nexus::voice::VoiceTurn (src/voice/voice_turn.h): one voice turn, from the microphone to the reply spoken.
// Pure logic: the OS's answers come in as arguments and the time is a number the test counts; no audio, no clock.

#include "voice/voice_turn.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace nexus::voice;
using K = Act::Kind;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

bool has(const std::vector<Act>& acts, K kind) {
    for (const auto& a : acts)
        if (a.kind == kind) return true;
    return false;
}

const Act* find(const std::vector<Act>& acts, K kind) {
    for (const auto& a : acts)
        if (a.kind == kind) return &a;
    return nullptr;
}

// A turn on emma's tab, with the microphone already granted.
VoiceTurn granted() {
    VoiceTurn t;
    t.context("emma", {{"emma", {}}, {"rowan", {}}});
    CHECK(t.press(0) == std::vector<Act>{{K::AskPermission}});
    CHECK(t.permission(true, "") == std::vector<Act>{{K::StartListening}});
    t.heard("", true); // nothing said: back to idle
    return t;
}

void theFirstPressAsksThenListens() {
    VoiceTurn t;
    t.context("emma", {{"emma", {}}});
    CHECK(t.press(0) == std::vector<Act>{{K::AskPermission}});
    CHECK(t.phase() == Phase::Asking);
    CHECK(t.press(1).empty()); // a second press while the OS asks does nothing
    CHECK(t.permission(true, "") == std::vector<Act>{{K::StartListening}});
    CHECK(t.phase() == Phase::Listening);
    CHECK(t.permitted() == Permission::Granted);
}

void aRefusalSaysSoWithTheFix() {
    VoiceTurn t;
    t.press(0);
    CHECK(t.permission(false, "").empty());
    CHECK(t.phase() == Phase::Denied);
    CHECK(t.line().find("Settings") != std::string::npos);
    CHECK(t.permission(true, "").empty()); // a stray answer when nothing asked changes nothing
    CHECK(t.phase() == Phase::Denied);
    // Pressing again asks the OS again (the person may have changed Settings); nothing listens until it says yes.
    CHECK(t.press(5) == std::vector<Act>{{K::AskPermission}});
    t.permission(false, "Speech recognition is off: allow it in Settings > Privacy & Security > Speech Recognition.");
    CHECK(t.line() == "Speech recognition is off: allow it in Settings > Privacy & Security > Speech Recognition.");
    CHECK(t.heard("Emma, hi", true).empty());
}

void theTranscriptFormsThenGoesToTheNamedAI() {
    auto t = granted();
    t.press(10);
    CHECK(t.heard("Rowan,", false).empty());
    CHECK(t.heardText() == "Rowan,");
    const auto acts = t.heard("Rowan, cut the release", true);
    CHECK((acts == std::vector<Act>{{K::StopListening}, {K::Open, "rowan"}, {K::Send, "rowan", "cut the release"}}));
    CHECK(t.line() == "To Rowan");
    CHECK(t.awaiting().count("rowan") == 1);
    CHECK(t.phase() == Phase::Idle);
    CHECK(t.heard("Rowan, again", true).empty()); // a late result from the recognizer is not sent twice
}

void noNameGoesToTheAIShowing() {
    auto t = granted();
    t.press(10);
    const auto acts = t.heard("how's the sprint?", true);
    CHECK((acts == std::vector<Act>{{K::StopListening}, {K::Send, "emma", "how's the sprint?"}}));
    CHECK(t.line() == "To Emma");
}

void justANameOpensTheTabAndSendsNothing() {
    auto t = granted();
    t.press(10);
    const auto acts = t.heard("Rowan.", true);
    CHECK((acts == std::vector<Act>{{K::StopListening}, {K::Open, "rowan"}}));
    CHECK(t.awaiting().empty());
    CHECK(t.line() == "Rowan");
}

void silenceSendsNothing() {
    auto t = granted();
    t.press(10);
    CHECK(t.heard("", true) == std::vector<Act>{{K::StopListening}});
    CHECK(t.line() == "Heard nothing; nothing was sent.");
}

void ambiguityGoesToTheDraft() {
    VoiceTurn t;
    t.context("emma", {{"emma", {"em"}}, {"emmett", {"em"}}});
    t.press(0);
    t.permission(true, "");
    const auto acts = t.heard("Em, lunch?", true);
    CHECK((acts == std::vector<Act>{{K::StopListening}, {K::Draft, "emma", "Em, lunch?"}}));
    CHECK(t.line().rfind("Emma or Emmett?", 0) == 0);
    CHECK(!has(acts, K::Send));
}

// The phone's list shows no AI: a prompt naming no one goes to the default AI, the first in the list.
void noNameAndNoAIShowingGoesToTheFirst() {
    VoiceTurn t;
    t.context("", {{"rowan", {}}, {"emma", {}}});
    t.press(0);
    t.permission(true, "");
    const auto acts = t.heard("what's left?", true);
    CHECK((acts == std::vector<Act>{{K::StopListening}, {K::Send, "rowan", "what's left?"}}));
    CHECK(t.line() == "To Rowan");
    CHECK(t.awaiting().count("rowan") == 1);
    // A name still picks its AI.
    t.press(1);
    CHECK((t.heard("Emma, and you?", true) ==
           std::vector<Act>{{K::StopListening}, {K::Open, "emma"}, {K::Send, "emma", "and you?"}}));
}

// No AI at all yet (the server not reached): nothing to send to, so the words go to the draft, and the line says so.
void noAIsAtAllGoesToTheDraft() {
    VoiceTurn t;
    t.context("", {});
    t.press(0);
    t.permission(true, "");
    const auto acts = t.heard("what's left?", true);
    CHECK((acts == std::vector<Act>{{K::StopListening}, {K::Draft, "", "what's left?"}}));
    CHECK(t.line().find("draft") != std::string::npos);
}

// A refusal and a failure mark the line as trouble (the prompt's dictation shows only those); a listen clears it.
void troubleIsMarked() {
    VoiceTurn t;
    t.context("emma", {});
    t.press(0);
    t.permission(false, "");
    CHECK(t.trouble());
    t.press(1);
    t.permission(true, "");
    CHECK(!t.trouble());
    t.failed("The audio session was interrupted.");
    CHECK(t.trouble());
    t.press(2);
    CHECK(!t.trouble());
    t.heard("status", true);
    CHECK(!t.trouble());
}

void pressingAgainEndsTheListen() {
    auto t = granted();
    t.press(10);
    t.heard("Emma, status", false);
    CHECK(t.press(12) == std::vector<Act>{{K::StopListening}});
    CHECK(t.phase() == Phase::Finishing);
    CHECK(t.press(12.5).empty());
    // The recognizer's final result after the stop is sent; no second stop.
    CHECK((t.heard("Emma, status.", true) == std::vector<Act>{{K::Open, "emma"}, {K::Send, "emma", "status."}}));
}

void aListenAlwaysEnds() {
    auto t = granted();
    t.press(100);
    t.heard("Emma, the long story", false);
    CHECK(t.tick(100 + VoiceTurn::kMaxListenSeconds - 0.01).empty());
    CHECK(t.tick(100 + VoiceTurn::kMaxListenSeconds) == std::vector<Act>{{K::StopListening}});
    CHECK(t.phase() == Phase::Finishing);
    const double stopped = 100 + VoiceTurn::kMaxListenSeconds;
    CHECK(t.tick(stopped + VoiceTurn::kFinishSeconds - 0.01).empty());
    // No final result came: what was heard is taken.
    CHECK((t.tick(stopped + VoiceTurn::kFinishSeconds) ==
           std::vector<Act>{{K::Open, "emma"}, {K::Send, "emma", "the long story"}}));
}

void aFailureKeepsWhatWasHeard() {
    auto t = granted();
    t.press(10);
    t.heard("Emma, the build is", false);
    const auto acts = t.failed("The audio session was interrupted by a call.");
    CHECK((acts == std::vector<Act>{{K::StopListening}, {K::Draft, "emma", "Emma, the build is"}}));
    CHECK(t.line() == "The audio session was interrupted by a call. What was heard is in the draft.");
    CHECK(t.phase() == Phase::Idle);
    CHECK(t.failed("again").empty());
}

void theReplyIsSpokenThenTheNext() {
    auto t = granted();
    t.press(10);
    t.heard("how's it going", true);
    CHECK((t.replied("emma", "Fine.") == std::vector<Act>{{K::Speak, "emma", "Fine."}}));
    CHECK(t.speaking());
    CHECK(t.replied("emma", "Two cards left.").empty()); // waits for the first
    CHECK(t.queued().size() == 1);
    CHECK((t.spoken() == std::vector<Act>{{K::Speak, "emma", "Two cards left."}}));
    CHECK(t.spoken().empty());
    CHECK(!t.speaking());
    t.turnEnded("emma");
    CHECK(t.replied("emma", "A later message.").empty()); // not an answer to a voice prompt
}

void onlyAnswersToVoiceAreSpoken() {
    auto t = granted();
    CHECK(t.replied("emma", "Typed prompt's reply").empty());
    CHECK(t.replied("rowan", "Someone else").empty());
    t.press(10);
    t.heard("Rowan, status", true);
    CHECK(t.replied("emma", "not asked").empty());
    CHECK(has(t.replied("rowan", "green"), K::Speak));
}

void listeningStopsTheSpeechFirst() {
    auto t = granted();
    t.press(10);
    t.heard("status", true);
    t.replied("emma", "Long answer");
    t.replied("emma", "More");
    CHECK(t.speaking());
    const auto acts = t.press(20);
    CHECK((acts == std::vector<Act>{{K::StopSpeaking}, {K::StartListening}}));
    CHECK(!t.speaking());
    CHECK(t.queued().empty());
    // A reply arriving while listening waits for the listen's end.
    CHECK(t.replied("emma", "Late block").empty());
    CHECK(!t.speaking());
    const auto done = t.heard("thanks", true);
    CHECK((done == std::vector<Act>{{K::StopListening}, {K::Send, "emma", "thanks"}, {K::Speak, "emma", "Late block"}}));
}

void textIsSilent() {
    auto t = granted();
    t.press(10);
    t.heard("status", true);
    t.replied("emma", "First");
    t.replied("emma", "Second");
    CHECK(t.setReply(Reply::Text) == std::vector<Act>{{K::StopSpeaking}});
    CHECK(!t.speaking());
    CHECK(t.queued().empty());
    CHECK(t.replied("emma", "Third").empty());
    CHECK(t.setReply(Reply::Voice).empty());
    CHECK(has(t.replied("emma", "Fourth"), K::Speak));
}

void namesForTheModel() {
    CHECK(std::string(phaseName(Phase::Finishing)) == "finishing");
    CHECK(std::string(replyName(Reply::Voice)) == "voice");
    // Replies are no setting: the menu speaks them, a tab texts them.
    CHECK(nexus::voice::replyOn(true) == Reply::Voice);
    CHECK(nexus::voice::replyOn(false) == Reply::Text);
    CHECK(find({{K::Open, "x"}}, K::Open)->aiName == "x");
}

} // namespace

int main() {
    theFirstPressAsksThenListens();
    aRefusalSaysSoWithTheFix();
    theTranscriptFormsThenGoesToTheNamedAI();
    noNameGoesToTheAIShowing();
    justANameOpensTheTabAndSendsNothing();
    silenceSendsNothing();
    ambiguityGoesToTheDraft();
    noNameAndNoAIShowingGoesToTheFirst();
    noAIsAtAllGoesToTheDraft();
    troubleIsMarked();
    pressingAgainEndsTheListen();
    aListenAlwaysEnds();
    aFailureKeepsWhatWasHeard();
    theReplyIsSpokenThenTheNext();
    onlyAnswersToVoiceAreSpoken();
    listeningStopsTheSpeechFirst();
    textIsSilent();
    namesForTheModel();
    if (failures) {
        std::fprintf(stderr, "%d voice-turn check(s) failed\n", failures);
        return 1;
    }
    std::printf("voice_turn: all checks passed\n");
    return 0;
}
