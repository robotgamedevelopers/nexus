// Which AI a spoken prompt is for: "Emma, how's the sprint?" goes to emma, with "how's the sprint?" as the prompt.
// Pure logic over a transcript and the server's list of AIs, with no Prax, no OS and no clock, so that the iPhone's
// window and a native watch client share it (docs/voice.md). Client-side input preparation only: the server sees an
// ordinary prompt sent to an ordinary session (AGENTS.md, the client is a thin client).
//
// An AI is named at the start of what was said, after at most two greetings ("hey Emma", "ok so Emma"), or at its
// end after a comma ("how's the sprint, Emma?"). Each AI answers to its spoken forms: the words of its name split
// at '-', '_' and '.' ("johnny-grok" is "johnny grok"), its first word alone when no other AI's name starts with
// that word, and any aliases the client's config gives it. Words are compared after case folding and folding Latin
// accents and pinyin tones to ASCII ("Zhì" is "zhi"); the recognizer's punctuation is ignored. The longest spoken form
// that matches wins; two AIs matching with forms of the same length is ambiguous, and nothing is guessed. A word
// of three letters or more, one letter away from exactly one AI's one-word form of four letters or more and
// followed by a comma (the recognizer's mark of a name being called), names that AI as well ("Ema, ..." is emma).
// A spoken form of two words or more also answers to words that sound the same, joined however the recognizer splits
// them: zh, ch, sh, j, q, x, z and s are one sound and i and e one vowel ("She Yawen" is zhi ya wen); the sound
// must be equal and six letters or more, in the same places a spelled name is looked for, after spelled matches fail.
// The prompt starts at the word after the name: the marks between ("Emma, ", "Emma — ") go.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nexus::voice {

struct SpokenAI {
    std::string name;                  // the AI's name as the server sends it (accounts::isName)
    std::vector<std::string> aliases;  // more spoken forms, from the client's config (voice_names)
};

enum class Route {
    Named,      // an AI was named: `aiName`, and `prompt` is what was said less the name and greeting
    Current,    // no AI was named: `prompt` is everything said, for the AI showing
    Ambiguous,  // two or more AIs answer equally: `candidates`, and nothing is sent
};

struct Routing {
    Route route = Route::Current;
    std::string aiName;
    std::string prompt;
    std::vector<std::string> candidates;
    bool operator==(const Routing&) const = default;
};

// The words of `text` folded for comparison: lower case ASCII, accents and tones dropped, anything else a separator.
std::vector<std::string> foldedWords(std::string_view text);

// The spoken forms of each AI, as above: each a list of folded words.
std::vector<std::vector<std::string>> spokenForms(const SpokenAI& who, const std::vector<SpokenAI>& all);

// Where `transcript` goes among `AIs`.
Routing route(std::string_view transcript, const std::vector<SpokenAI>& ais);

} // namespace nexus::voice
