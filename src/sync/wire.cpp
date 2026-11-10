#include "sync/wire.h"

namespace nexus::sync {

void Writer::u64(std::uint64_t v) {
    while (v >= 0x80) {
        out_.push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out_.push_back(static_cast<char>(v));
}

void Writer::byte(std::uint8_t v) {
    out_.push_back(static_cast<char>(v));
}

void Writer::str(const std::string& s) {
    u64(s.size());
    out_ += s;
}

void Writer::raw(const char* data, std::size_t size) {
    out_.append(data, size);
}

std::uint64_t Reader::u64() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (at_ == in_.size())
            throw BadDiff("diff ends inside a number");
        const auto b = static_cast<std::uint8_t>(in_[at_++]);
        if (shift == 63 && (b & 0x7e) != 0)
            throw BadDiff("diff holds a number past 64 bits");
        v |= std::uint64_t(b & 0x7f) << shift;
        if ((b & 0x80) == 0)
            return v;
    }
    throw BadDiff("diff holds a number past 64 bits");
}

std::uint8_t Reader::byte() {
    if (at_ == in_.size())
        throw BadDiff("diff ends where a byte was due");
    return static_cast<std::uint8_t>(in_[at_++]);
}

std::string Reader::str() {
    const std::uint64_t n = u64();
    if (n > in_.size() - at_)
        throw BadDiff("diff ends inside a string of " + std::to_string(n) + " bytes");
    std::string s = in_.substr(at_, static_cast<std::size_t>(n));
    at_ += static_cast<std::size_t>(n);
    return s;
}

void Reader::end() const {
    if (at_ != in_.size())
        throw BadDiff("diff has " + std::to_string(in_.size() - at_) + " bytes past its end");
}

} // namespace nexus::sync
