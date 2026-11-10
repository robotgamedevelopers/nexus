#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nexus::store {

using Bytes = std::vector<std::uint8_t>;
using Hash = std::array<std::uint8_t, 32>;  // SHA-256

// CRC-32C (Castagnoli), as in iSCSI and ext4: the checksum of every log record and snapshot. `crc` continues an
// earlier one; start at 0.
std::uint32_t crc32c(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

// SHA-256 of `data`; a blob is named by it.
class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    Hash finish();

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};
Hash sha256(std::span<const std::uint8_t> data);
std::string to_hex(std::span<const std::uint8_t> data);

// Little-endian encoding of the store's records and snapshots.
class Writer {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void bytes(std::span<const std::uint8_t> v);  // u32 length, then the bytes
    void str(const std::string& v);               // as bytes
    void raw(std::span<const std::uint8_t> v) { out_.insert(out_.end(), v.begin(), v.end()); }
    Bytes& out() { return out_; }

private:
    Bytes out_;
};

// Reads what Writer wrote. A read past the end returns zeros and clears ok(); callers check ok() once at the end.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> in) : in_(in) {}
    std::uint8_t u8();
    std::uint32_t u32();
    std::uint64_t u64();
    Bytes bytes();
    std::string str();
    void raw(std::uint8_t* out, std::size_t n);
    bool ok() const { return ok_; }
    bool done() const { return pos_ == in_.size(); }

private:
    bool take(std::size_t n);
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

void put_u32(std::uint8_t* p, std::uint32_t v);
void put_u64(std::uint8_t* p, std::uint64_t v);
std::uint32_t get_u32(const std::uint8_t* p);
std::uint64_t get_u64(const std::uint8_t* p);

} // namespace nexus::store
