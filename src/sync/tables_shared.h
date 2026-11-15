// Internal to the format 4 codec (sync/codec.h): what tables_codec.cpp (the sessions) and tables_actions.cpp (the
// actions) share. Two translation units, so that the two halves of the codec, each over schema's large generated
// header, compile side by side.

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "sync/codec.h"
#include "wire/tables.h"

namespace nexus::sync::codec::shared {

namespace wire = nexuswire;

// The length of the UTF-8 sequence at text[i] when it is well formed and not a zero byte, else 0 (the rules of
// TableUtf8Valid: no overlong form, no surrogate, nothing past U+10FFFF).
inline std::size_t sequenceAt(std::string_view text, std::size_t i) {
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = at(i);
    if (lead == 0) {
        return 0;
    }
    if (lead < 0x80) {
        return 1;
    }
    std::size_t n = 0;
    std::uint32_t cp = 0;
    std::uint32_t least = 0;
    if ((lead & 0xE0) == 0xC0) {
        n = 2;
        cp = lead & 0x1Fu;
        least = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        n = 3;
        cp = lead & 0x0Fu;
        least = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        n = 4;
        cp = lead & 0x07u;
        least = 0x10000;
    } else {
        return 0;
    }
    if (text.size() - i < n) {
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        const unsigned char c = at(i + k);
        if ((c & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (c & 0x3Fu);
    }
    if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return n;
}

// `text` as a table's text: as it is when it already is one, else with each byte that starts no well-formed sequence
// (or is zero) replaced by U+FFFD.
inline std::string_view asText(std::string_view text, std::string& scratch) {
    if (tableText(text)) {
        return text;
    }
    scratch.clear();
    scratch.reserve(text.size() + 8);
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t n = sequenceAt(text, i);
        if (n == 0) {
            scratch += "\xEF\xBF\xBD";
            ++i;
        } else {
            scratch.append(text.data() + i, n);
            i += n;
        }
    }
    return scratch;
}

// A *string field: absent when empty (which reads as empty), else the text.
inline void putText(wire::TableWorker& worker, wire::TableRef& slot, std::string_view text, std::string& scratch) {
    if (text.empty()) {
        return;
    }
    const std::string_view t = asText(text, scratch);
    if (wire::TableStringEmplace(worker, slot, t.data(), static_cast<int64_t>(t.size())) == nullptr) {
        throw std::bad_alloc();
    }
}

inline std::string textOf(const wire::TableRef& ref) {
    const wire::TableStringView v = wire::TableStringAt(ref);
    return v.data == nullptr ? std::string() : std::string(v.data, static_cast<std::size_t>(v.length));
}

template <typename T>
T* added(T* node) {
    if (node == nullptr) {
        throw std::bad_alloc();
    }
    return node;
}

// The bytes of a builder: measured, then saved into a string of exactly that size.
template <typename Builder, typename Measure, typename Save>
std::string saved(const Builder& b, Measure measure, Save save, const char* what) {
    const int64_t need = measure(b);
    if (need <= 0) {
        throw std::logic_error(std::string(what) + ": schema's measure failed (" + std::to_string(need) + ")");
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    const int64_t wrote = save(b, reinterpret_cast<std::uint8_t*>(out.data()), need);
    if (wrote != need) {
        throw std::logic_error(std::string(what) + ": schema's save wrote " + std::to_string(wrote) + " of " +
            std::to_string(need) + " bytes");
    }
    return out;
}

// The region a load reads into, wiped when it goes: it may hold a secret's value.
struct Region {
    std::vector<std::uint8_t> bytes;
    ~Region() {
        volatile std::uint8_t* p = bytes.data();
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            p[i] = 0;
        }
    }
};

// The region a read of `diff` loads into, by the generated LoadMeasure: BadDiff when the diff is longer than the link
// carries (kMaxDiffBytes), when it does not measure, or when its region is past kMaxReadBytes (sync/codec.h), so a read
// never allocates more than that.
template <typename Measure>
std::size_t readNeed(const std::string& diff, Measure measure, const char* what) {
    checkDiffSize(diff.size(), what);
    const int64_t need = measure(reinterpret_cast<const std::uint8_t*>(diff.data()), static_cast<int64_t>(diff.size()));
    checkReadSize(diff.size(), need, what);
    return static_cast<std::size_t>(need);
}

} // namespace nexus::sync::codec::shared
