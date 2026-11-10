#include "store/codec.h"

#include <algorithm>
#include <cstring>

namespace nexus::store {

namespace {

struct Crc32cTable {
    std::uint32_t t[256];
    Crc32cTable() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            t[i] = c;
        }
    }
};

const Crc32cTable kCrc;

} // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> data, std::uint32_t crc) {
    crc = ~crc;
    for (std::uint8_t b : data)
        crc = kCrc.t[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        p[i] = std::uint8_t(v >> (8 * i));
}

void put_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        p[i] = std::uint8_t(v >> (8 * i));
}

std::uint32_t get_u32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= std::uint32_t(p[i]) << (8 * i);
    return v;
}

std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= std::uint64_t(p[i]) << (8 * i);
    return v;
}

void Writer::u32(std::uint32_t v) {
    std::uint8_t b[4];
    put_u32(b, v);
    out_.insert(out_.end(), b, b + 4);
}

void Writer::u64(std::uint64_t v) {
    std::uint8_t b[8];
    put_u64(b, v);
    out_.insert(out_.end(), b, b + 8);
}

void Writer::bytes(std::span<const std::uint8_t> v) {
    u32(std::uint32_t(v.size()));
    raw(v);
}

void Writer::str(const std::string& v) {
    bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(v.data()), v.size()));
}

bool Reader::take(std::size_t n) {
    if (!ok_ || in_.size() - pos_ < n) {
        ok_ = false;
        return false;
    }
    return true;
}

std::uint8_t Reader::u8() {
    if (!take(1))
        return 0;
    return in_[pos_++];
}

std::uint32_t Reader::u32() {
    if (!take(4))
        return 0;
    std::uint32_t v = get_u32(in_.data() + pos_);
    pos_ += 4;
    return v;
}

std::uint64_t Reader::u64() {
    if (!take(8))
        return 0;
    std::uint64_t v = get_u64(in_.data() + pos_);
    pos_ += 8;
    return v;
}

Bytes Reader::bytes() {
    std::uint32_t n = u32();
    if (!take(n))
        return {};
    Bytes v(in_.begin() + std::ptrdiff_t(pos_), in_.begin() + std::ptrdiff_t(pos_ + n));
    pos_ += n;
    return v;
}

std::string Reader::str() {
    Bytes b = bytes();
    return std::string(b.begin(), b.end());
}

void Reader::raw(std::uint8_t* out, std::size_t n) {
    if (!take(n)) {
        std::memset(out, 0, n);
        return;
    }
    std::memcpy(out, in_.data() + pos_, n);
    pos_ += n;
}

} // namespace nexus::store
