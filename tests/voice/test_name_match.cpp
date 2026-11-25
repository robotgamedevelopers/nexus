// Unit tests of nexus::voice::route (src/voice/name_match.h): which AI a spoken prompt is for. Pure logic: no OS, no
// time, no audio. The transcripts are written as Apple's recognizer writes them: capitals, commas and question marks
// where it hears them, and accents on names it knows.

#include "voice/name_match.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace nexus::voice;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

const std::vector<SpokenAI> kAIs = {
    {"emma", {}}, {"rowan", {}}, {"johnny-grok", {}}, {"zhi", {"zhi ya wen"}}, {"stella", {}},
};

Routing to(std::string_view said, const std::vector<SpokenAI>& ais = kAIs) {
    return route(said, ais);
}

bool named(const Routing& r, std::string_view who, std::string_view prompt) {
    if (r.route != Route::Named || r.aiName != who || r.prompt != prompt) {
        std::fprintf(stderr, "  routed to '%s' (%d) with '%s'\n", r.aiName.c_str(), int(r.route), r.prompt.c_str());
        return false;
    }
    return true;
}

void aNameFirstRoutesTheRest() {
    CHECK(named(to("Emma, how's the sprint?"), "emma", "how's the sprint?"));
    CHECK(named(to("emma how's the sprint"), "emma", "how's the sprint"));
    CHECK(named(to("EMMA. Status please."), "emma", "Status please."));
}

void greetingsBeforeTheName() {
    CHECK(named(to("Hey Emma, how's the sprint?"), "emma", "how's the sprint?"));
    CHECK(named(to("OK so Rowan, cut the release"), "rowan", "cut the release"));
    CHECK(named(to("Hi, Stella: are you there?"), "stella", "are you there?"));
    // Three greetings is talk, not a call: it stays with the AI showing.
    CHECK(to("Hey hi hello Emma what's up").route == Route::Current);
}

void aNameLastAfterAComma() {
    CHECK(named(to("How's the sprint, Emma?"), "emma", "How's the sprint?"));
    CHECK(named(to("what's next, Johnny Grok"), "johnny-grok", "what's next"));
    // Without the comma a name at the end is part of the sentence, not a call.
    CHECK(to("Tell me about Emma").route == Route::Current);
}

void noNameIsTheCurrentAI() {
    const auto r = to("  What's left on the board?  ");
    CHECK(r.route == Route::Current);
    CHECK(r.prompt == "What's left on the board?");
    CHECK(r.aiName.empty());
    // A name inside the sentence routes nothing.
    CHECK(to("Ask whether emma finished").route == Route::Current);
}

void spokenVariantsOfAName() {
    // A name of several words is said in words, and its first word alone names it when no other name starts so.
    CHECK(named(to("Johnny Grok, run the tests"), "johnny-grok", "run the tests"));
    CHECK(named(to("Johnny, run the tests"), "johnny-grok", "run the tests"));
    // Accents and tones, as the recognizer writes a name it knows, fold away.
    CHECK(named(to("Zhì, nǐ hǎo"), "zhi", "nǐ hǎo"));
    // The longest form wins: the alias of three words over the name's one.
    CHECK(named(to("Zhi Ya Wen, the proof?"), "zhi", "the proof?"));
    // Typographic apostrophes and decomposed accents are part of their word.
    CHECK(to("Emma’s turn: go").route == Route::Current);
    CHECK(named(to("Stélla, hi"), "stella", "hi"));
}

void justTheNameOpensTheTab() {
    CHECK(named(to("Rowan."), "rowan", ""));
    CHECK(named(to("hey Emma"), "emma", ""));
}

void similarNamesAreKeptApart() {
    const std::vector<SpokenAI> ais = {{"emma", {}}, {"emmett", {}}, {"alex", {}}, {"alexa", {}}};
    CHECK(named(to("Emma, status", ais), "emma", "status"));
    CHECK(named(to("Emmett, status", ais), "emmett", "status"));
    CHECK(named(to("Alexa, status", ais), "alexa", "status"));
    CHECK(named(to("Alex, status", ais), "alex", "status"));
    // One letter off both "alex" and "alexa": nothing is guessed.
    const auto r = to("Alexi, status", ais);
    CHECK(r.route == Route::Ambiguous);
    CHECK((r.candidates == std::vector<std::string>{"alex", "alexa"}));
}

void twoAIsSharingAFirstWord() {
    const std::vector<SpokenAI> ais = {{"johnny-grok", {}}, {"johnny-cash", {}}};
    // The first word alone is no one's, since both start with it.
    CHECK(to("Johnny, play", ais).route == Route::Current);
    CHECK(named(to("Johnny Cash, play", ais), "johnny-cash", "play"));
}

void aSharedAliasIsAmbiguous() {
    const std::vector<SpokenAI> ais = {{"emma", {"em"}}, {"emmett", {"em"}}};
    const auto r = to("Em, are you free?", ais);
    CHECK(r.route == Route::Ambiguous);
    CHECK((r.candidates == std::vector<std::string>{"emma", "emmett"}));
    CHECK(r.prompt == "Em, are you free?");
}

void aMisheardNameCalledWithAComma() {
    CHECK(named(to("Ema, how's the sprint?"), "emma", "how's the sprint?"));
    CHECK(named(to("Hey Stela, hi"), "stella", "hi"));
    // Without the comma, or for a short name, no guess.
    CHECK(to("Ema how's the sprint").route == Route::Current);
    CHECK(to("Zhe, hello").route == Route::Current);
}

void punctuationFromTheRecognizer() {
    CHECK(named(to("Emma — what's up?"), "emma", "what's up?"));
    CHECK(named(to("“Emma”, what's up?"), "emma", "what's up?"));
    CHECK(named(to("Emma，你好"), "emma", "你好"));
    CHECK(to("...").route == Route::Current);
    CHECK(to("").prompt.empty());
}

void noAIsRoutesNothing() {
    const auto r = to("Emma, hi", {});
    CHECK(r.route == Route::Current);
    CHECK(r.prompt == "Emma, hi");
}

void foldingAndForms() {
    CHECK((foldedWords("Zhì Yǎ Wén!") == std::vector<std::string>{"zhi", "ya", "wen"}));
    CHECK((foldedWords("Ærø's café") == std::vector<std::string>{"aeros", "cafe"}));
    const auto forms = spokenForms({"johnny-grok", {"JG"}}, {{"johnny-grok", {}}});
    CHECK((forms == std::vector<std::vector<std::string>>{{"johnny", "grok"}, {"johnny"}, {"jg"}}));
}

} // namespace

int main() {
    aNameFirstRoutesTheRest();
    greetingsBeforeTheName();
    aNameLastAfterAComma();
    noNameIsTheCurrentAI();
    spokenVariantsOfAName();
    justTheNameOpensTheTab();
    similarNamesAreKeptApart();
    twoAIsSharingAFirstWord();
    aSharedAliasIsAmbiguous();
    aMisheardNameCalledWithAComma();
    punctuationFromTheRecognizer();
    noAIsRoutesNothing();
    foldingAndForms();
    if (failures) {
        std::fprintf(stderr, "%d name-match check(s) failed\n", failures);
        return 1;
    }
    std::printf("name_match: all checks passed\n");
    return 0;
}
