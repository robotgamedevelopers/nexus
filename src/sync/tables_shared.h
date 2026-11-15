// Internal to the format 4 codec (sync/codec.h): what tables_codec.cpp (the sessions) and tables_actions.cpp (the
// actions) share. Two translation units, so that the two halves of the codec, each over schema's large generated
// header, compile side by side.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

// A builder's memory, every byte overwritten when it is freed: for a diff that holds a secret's value (an action's) or
// a sign-in's URL (an offer's), which the builder copied into its arena (sync/secrets.h: where a value may be). Each
// block keeps its size in front of it.
constexpr std::size_t kWipeHeader = 16;

inline void* wipingAlloc(void*, int64_t bytes) {
    if (bytes <= 0) {
        return nullptr;
    }
    const std::size_t size = static_cast<std::size_t>(bytes);
    auto* block = static_cast<unsigned char*>(std::calloc(1, size + kWipeHeader));
    if (block == nullptr) {
        return nullptr;
    }
    std::memcpy(block, &size, sizeof size);
    return block + kWipeHeader;
}

inline void wipingFree(void*, void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    unsigned char* block = static_cast<unsigned char*>(pointer) - kWipeHeader;
    std::size_t size = 0;
    std::memcpy(&size, block, sizeof size);
    volatile unsigned char* bytes = block;
    for (std::size_t i = 0; i < size + kWipeHeader; ++i) {
        bytes[i] = 0;
    }
    std::free(block);
}

inline wire::TableAllocator wiping() {
    return wire::TableAllocator{wipingAlloc, wipingFree, nullptr};
}

// `value` as a *bytes field: absent when empty, else its bytes as they are, never changed (a secret's value, a
// sign-in's URL). Never text: putText would rewrite a malformed byte.
inline void putBytes(wire::TableWorker& worker, wire::TableRef& slot, const std::string& value) {
    if (value.empty()) {
        return;
    }
    std::uint8_t* to = wire::TableBytesEmplace(worker, slot, static_cast<int64_t>(value.size()));
    if (to == nullptr) {
        throw std::bad_alloc();
    }
    std::memcpy(to, value.data(), value.size());
}

// A *bytes field read back as a value; empty when absent.
inline SecretValue bytesOf(const wire::TableRef& ref) {
    const wire::TableBytesView v = wire::TableBytesAt(ref);
    if (v.data == nullptr || v.length <= 0) {
        return SecretValue();
    }
    return SecretValue(std::string_view(reinterpret_cast<const char*>(v.data), static_cast<std::size_t>(v.length)));
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
