// SHA-256 (FIPS 180-4): the content hash a placed build is named by (server/builds.h). Pure: bytes in, digest out.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nexus::server {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    void update(const std::string& text) { update(text.data(), text.size()); }
    // The digest of everything given; the object is spent after (make another for the next).
    std::array<std::uint8_t, 32> digest();
    // The digest as 64 lowercase hex digits.
    std::string hex();

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

// The hex digest of `text`.
std::string sha256Hex(const std::string& text);

} // namespace nexus::server
