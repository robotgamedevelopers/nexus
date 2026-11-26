#include "voice/name_match.h"

#include <algorithm>
#include <cstdint>

namespace nexus::voice {
namespace {

// U+0100 to U+017F (Latin Extended-A), each the ASCII letter it folds to.
constexpr std::string_view kExtendedA =
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkkllllllllllnnnnnnnnnooooooo"
    "orrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
// U+01CD to U+01DC: the pinyin vowels with tones (and u with diaeresis).
constexpr std::string_view kPinyin = "aaiioouuuuuuuuuu";

static_assert(kExtendedA.size() == 0x80);
static_assert(kPinyin.size() == 0x10);

// What a code point folds to: its ASCII letters, "" when it is dropped inside a word (an apostrophe, a combining
// mark), or nullopt-like "\x01" when it separates words.
constexpr std::string_view kSeparator = "\x01";

std::string_view fold(uint32_t cp) {
    static const char* const lower = "abcdefghijklmnopqrstuvwxyz";
    static const char* const digits = "0123456789";
    if (cp >= 'a' && cp <= 'z') return {lower + (cp - 'a'), 1};
    if (cp >= 'A' && cp <= 'Z') return {lower + (cp - 'A'), 1};
    if (cp >= '0' && cp <= '9') return {digits + (cp - '0'), 1};
    if (cp == '\'' || cp == 0x2019 || (cp >= 0x300 && cp <= 0x36f)) return {};
    if (cp >= 0xc0 && cp <= 0xff && cp != 0xd7 && cp != 0xf7) {
        //                                    À       Æ Ç È   Ì   Ð Ñ Ò     Ø Ù   Ý Þ ß
        static constexpr std::string_view latin1 = "aaaaaaaceeeeiiiidnooooo_ouuuuyts";
        const char c = latin1[(cp - 0xc0) & 0x1f];
        if (cp == 0xc6 || cp == 0xe6) return "ae";
        if (cp == 0xde || cp == 0xfe) return "th";
        if (cp == 0xdf) return "ss";
        if (cp == 0xff) return "y";
        return {lower + (c - 'a'), 1};
    }
    if (cp >= 0x100 && cp <= 0x17f) {
        const char c = kExtendedA[cp - 0x100];
        return {lower + (c - 'a'), 1};
    }
    if (cp >= 0x1cd && cp <= 0x1dc) {
        const char c = kPinyin[cp - 0x1cd];
        return {lower + (c - 'a'), 1};
    }
    if (cp < 0x80 || (cp >= 0x2000 && cp <= 0x206f) || (cp >= 0x3000 && cp <= 0x303f) || (cp >= 0xff00 && cp <= 0xff0f))
        return kSeparator; // ASCII punctuation and space, general punctuation, CJK and full-width punctuation
    return "\x02";         // any other letter: kept as its own bytes
}

// One UTF-8 code point at `at`, and its length; a byte that does not start one is taken alone as U+FFFD.
uint32_t decode(std::string_view s, std::size_t at, std::size_t& length) {
    const auto b = static_cast<unsigned char>(s[at]);
    auto cont = [&](std::size_t i) { return at + i < s.size() && (static_cast<unsigned char>(s[at + i]) & 0xc0) == 0x80; };
    auto bits = [&](std::size_t i) { return uint32_t(static_cast<unsigned char>(s[at + i]) & 0x3f); };
    if (b < 0x80) { length = 1; return b; }
    if ((b & 0xe0) == 0xc0 && cont(1)) { length = 2; return (uint32_t(b & 0x1f) << 6) | bits(1); }
    if ((b & 0xf0) == 0xe0 && cont(1) && cont(2)) { length = 3; return (uint32_t(b & 0x0f) << 12) | (bits(1) << 6) | bits(2); }
    if ((b & 0xf8) == 0xf0 && cont(1) && cont(2) && cont(3)) {
        length = 4;
        return (uint32_t(b & 0x07) << 18) | (bits(1) << 12) | (bits(2) << 6) | bits(3);
    }
    length = 1;
    return 0xfffd;
}

struct Word {
    std::string folded;
    std::size_t begin = 0, end = 0; // its bytes in the transcript
    bool commaAfter = false;        // the next mark after it, before any other word, is a comma
};

std::vector<Word> words(std::string_view text) {
    std::vector<Word> out;
    Word current;
    bool in = false;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t length = 1;
        const uint32_t cp = decode(text, at, length);
        const auto f = fold(cp);
        if (f == kSeparator) {
            if (in) {
                current.end = at;
                out.push_back(std::move(current));
                current = {};
                in = false;
            }
            if ((cp == ',' || cp == 0xff0c || cp == 0x3001) && !out.empty()) out.back().commaAfter = true;
        } else {
            if (!in) {
                current.begin = at;
                in = true;
            }
            if (f == "\x02") current.folded.append(text.substr(at, length));
            else current.folded.append(f);
        }
        at += length;
    }
    if (in) {
        current.end = text.size();
        out.push_back(std::move(current));
    }
    // A word of nothing but dropped marks (a lone apostrophe) is no word.
    std::erase_if(out, [](const Word& w) { return w.folded.empty(); });
    return out;
}

bool greeting(std::string_view w) {
    static constexpr std::string_view kGreetings[] = {"hey", "hi", "hello", "ok", "okay", "yo", "oh", "um", "uh", "so",
        "and", "dear"};
    return std::find(std::begin(kGreetings), std::end(kGreetings), w) != std::end(kGreetings);
}

bool oneApart(std::string_view a, std::string_view b) {
    if (a.size() > b.size()) std::swap(a, b);
    if (b.size() - a.size() > 1) return false;
    std::size_t i = 0;
    while (i < a.size() && a[i] == b[i]) ++i;
    if (a.size() == b.size()) return i < a.size() && a.substr(i + 1) == b.substr(i + 1);
    return a.substr(i) == b.substr(i + 1);
}

std::string_view trim(std::string_view s, std::string_view marks) {
    const auto first = s.find_first_not_of(marks);
    if (first == s.npos) return {};
    const auto last = s.find_last_not_of(marks);
    return s.substr(first, last - first + 1);
}

constexpr std::string_view kSpace = " \t\r\n";

struct Found {
    std::vector<std::string> ais; // those answering with the longest form
    std::size_t length = 0;            // in words
};

// The AIs whose spoken forms match the words at [at, at + length), the longest forms only.
Found matchAt(const std::vector<Word>& said, std::size_t at, std::size_t limit,
    const std::vector<std::vector<std::vector<std::string>>>& forms, const std::vector<SpokenAI>& ais,
    bool endsAtLimit) {
    Found found;
    for (std::size_t f = 0; f < ais.size(); ++f) {
        for (const auto& form : forms[f]) {
            if (form.empty() || at + form.size() > limit) continue;
            if (endsAtLimit && at + form.size() != limit) continue;
            bool same = true;
            for (std::size_t i = 0; i < form.size() && same; ++i) same = said[at + i].folded == form[i];
            if (!same || form.size() < found.length) continue;
            if (form.size() > found.length) {
                found.ais.clear();
                found.length = form.size();
            }
            if (std::find(found.ais.begin(), found.ais.end(), ais[f].name) == found.ais.end())
                found.ais.push_back(ais[f].name);
        }
    }
    return found;
}

// How a run of folded letters sounds, coarsely, for names the recognizer spells as words it knows ("Zhi Ya Wen" heard
// as "She Yawen"): the hushed and hissed initials zh, ch, sh, j, q, x, z and s are one sound, and i and e are one
// vowel. The words are joined first, so the split the recognizer chooses ("Ya Wen", "Yawen") does not matter.
std::string soundOf(std::string_view joined) {
    std::string key;
    for (std::size_t i = 0; i < joined.size(); ++i) {
        const char c = joined[i];
        const char next = i + 1 < joined.size() ? joined[i + 1] : '\0';
        if ((c == 'z' || c == 'c' || c == 's') && next == 'h') {
            key += 'S';
            ++i;
        } else if (c == 'j' || c == 'q' || c == 'x' || c == 'z' || c == 's') {
            key += 'S';
        } else if (c == 'i' || c == 'e') {
            key += 'E';
        } else {
            key += c;
        }
    }
    return key;
}

std::string joined(const std::vector<std::string>& form) {
    std::string out;
    for (const auto& w : form) out += w;
    return out;
}

// The AIs one of whose spoken forms of two words or more sounds like the words at [at, at + n), for some n up to one
// more than the form's words, with `length` the n that matched. Only forms of several words take part: a name of
// one word is matched as spelled, or one letter off (route), and a sound alone is too loose for it. The sound must be
// equal, not near, and at least six long, so ordinary speech does not name an AI.
Found soundsAt(const std::vector<Word>& said, std::size_t at, const std::vector<std::vector<std::vector<std::string>>>& forms,
    const std::vector<SpokenAI>& ais, bool endsAtEnd) {
    Found found;
    for (std::size_t f = 0; f < ais.size(); ++f) {
        for (const auto& form : forms[f]) {
            if (form.size() < 2) continue;
            const auto sound = soundOf(joined(form));
            if (sound.size() < 6) continue;
            std::string heard;
            for (std::size_t n = 1; n <= form.size() + 1 && at + n <= said.size(); ++n) {
                heard += said[at + n - 1].folded;
                if (endsAtEnd && at + n != said.size()) continue;
                if (soundOf(heard) != sound) continue;
                if (std::find(found.ais.begin(), found.ais.end(), ais[f].name) == found.ais.end()) {
                    found.ais.push_back(ais[f].name);
                    found.length = std::max(found.length, n);
                }
                break;
            }
        }
    }
    return found;
}

} // namespace

std::vector<std::string> foldedWords(std::string_view text) {
    std::vector<std::string> out;
    for (auto& w : words(text)) out.push_back(std::move(w.folded));
    return out;
}

std::vector<std::vector<std::string>> spokenForms(const SpokenAI& who, const std::vector<SpokenAI>& all) {
    std::vector<std::vector<std::string>> forms;
    auto add = [&](std::vector<std::string> form) {
        if (!form.empty() && std::find(forms.begin(), forms.end(), form) == forms.end()) forms.push_back(std::move(form));
    };
    const auto whole = foldedWords(who.name);
    add(whole);
    if (whole.size() > 1) {
        const bool shared = std::any_of(all.begin(), all.end(), [&](const SpokenAI& other) {
            if (other.name == who.name) return false;
            const auto theirs = foldedWords(other.name);
            return !theirs.empty() && theirs.front() == whole.front();
        });
        if (!shared) add({whole.front()});
    }
    for (const auto& alias : who.aliases) add(foldedWords(alias));
    return forms;
}

Routing route(std::string_view transcript, const std::vector<SpokenAI>& ais) {
    const auto said = words(transcript);
    Routing out;
    out.prompt = std::string(trim(transcript, kSpace));
    if (said.empty() || ais.empty()) return out;
    std::vector<std::vector<std::vector<std::string>>> forms;
    for (const auto& f : ais) forms.push_back(spokenForms(f, ais));

    auto named = [&](const std::string& name, std::string prompt) {
        out.route = Route::Named;
        out.aiName = name;
        out.prompt = std::move(prompt);
        return out;
    };
    // What was said from the word at `next` on: the marks between a name and the prompt ("Emma, ", "Emma — ") go.
    auto after = [&](std::size_t next) {
        return next < said.size() ? std::string(trim(transcript.substr(said[next].begin), kSpace)) : std::string();
    };
    auto ambiguous = [&](std::vector<std::string> who) {
        out.route = Route::Ambiguous;
        out.candidates = std::move(who);
        return out;
    };

    // At the start, after at most two greetings.
    for (std::size_t at = 0; at < said.size() && at <= 2; ++at) {
        const auto found = matchAt(said, at, said.size(), forms, ais, false);
        if (found.ais.size() > 1) return ambiguous(found.ais);
        if (found.ais.size() == 1) {
            return named(found.ais.front(), after(at + found.length));
        }
        if (!greeting(said[at].folded)) break;
    }
    // At the end, after a comma: "how's the sprint, Emma?"
    for (std::size_t at = 1; at < said.size(); ++at) {
        if (!said[at - 1].commaAfter) continue;
        const auto found = matchAt(said, at, said.size(), forms, ais, true);
        if (found.ais.size() > 1) return ambiguous(found.ais);
        if (found.ais.size() == 1) {
            const auto before = trim(transcript.substr(0, said[at].begin), std::string_view(" \t\r\n,"));
            const auto after = trim(transcript.substr(said.back().end), kSpace);
            return named(found.ais.front(), std::string(before) + std::string(after));
        }
    }
    // A name of several words that sounds right though spelled otherwise, at the start or at the end after a comma:
    // the recognizer's spelling of a name it does not know ("She Yawen, ..." is zhi ya wen).
    for (std::size_t at = 0; at < said.size() && at <= 2; ++at) {
        const auto found = soundsAt(said, at, forms, ais, false);
        if (found.ais.size() > 1) return ambiguous(found.ais);
        if (found.ais.size() == 1) return named(found.ais.front(), after(at + found.length));
        if (!greeting(said[at].folded)) break;
    }
    for (std::size_t at = 1; at < said.size(); ++at) {
        if (!said[at - 1].commaAfter) continue;
        const auto found = soundsAt(said, at, forms, ais, true);
        if (found.ais.size() > 1) return ambiguous(found.ais);
        if (found.ais.size() == 1) {
            const auto before = trim(transcript.substr(0, said[at].begin), std::string_view(" \t\r\n,"));
            const auto after = trim(transcript.substr(said.back().end), kSpace);
            return named(found.ais.front(), std::string(before) + std::string(after));
        }
    }
    // One letter off a one-word name, called with a comma, at the start: the recognizer's spelling of a name it
    // does not know ("Ema, ...").
    for (std::size_t at = 0; at < said.size() && at <= 2; ++at) {
        const auto& w = said[at];
        if (w.commaAfter && w.folded.size() >= 3) {
            std::vector<std::string> near;
            for (std::size_t f = 0; f < ais.size(); ++f)
                for (const auto& form : forms[f])
                    if (form.size() == 1 && form.front().size() >= 4 && oneApart(form.front(), w.folded) &&
                            std::find(near.begin(), near.end(), ais[f].name) == near.end())
                        near.push_back(ais[f].name);
            if (near.size() == 1) return named(near.front(), after(at + 1));
            if (near.size() > 1) return ambiguous(near);
        }
        if (!greeting(w.folded)) break;
    }
    return out;
}

} // namespace nexus::voice
