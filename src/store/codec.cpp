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

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

} // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> data, std::uint32_t crc) {
    crc = ~crc;
    for (std::uint8_t b : data)
        crc = kCrc.t[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, buf_{} {}

void Sha256::block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) {
    total_ += data.size();
    std::size_t i = 0;
    while (i < data.size()) {
        std::size_t n = std::min<std::size_t>(64 - used_, data.size() - i);
        std::memcpy(buf_ + used_, data.data() + i, n);
        used_ += n;
        i += n;
        if (used_ == 64) {
            block(buf_);
            used_ = 0;
        }
    }
}

Hash Sha256::finish() {
    std::uint64_t bits = total_ * 8;
    std::uint8_t pad = 0x80;
    update(std::span<const std::uint8_t>(&pad, 1));
    std::uint8_t zero = 0;
    while (used_ != 56)
        update(std::span<const std::uint8_t>(&zero, 1));
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i)
        len[i] = std::uint8_t(bits >> (56 - 8 * i));
    update(std::span<const std::uint8_t>(len, 8));
    Hash out{};
    for (int i = 0; i < 8; ++i) {
        out[std::size_t(4 * i)] = std::uint8_t(h_[i] >> 24);
        out[std::size_t(4 * i + 1)] = std::uint8_t(h_[i] >> 16);
        out[std::size_t(4 * i + 2)] = std::uint8_t(h_[i] >> 8);
        out[std::size_t(4 * i + 3)] = std::uint8_t(h_[i]);
    }
    return out;
}

Hash sha256(std::span<const std::uint8_t> data) {
    Sha256 s;
    s.update(data);
    return s.finish();
}

std::string to_hex(std::span<const std::uint8_t> data) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::uint8_t b : data) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
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
