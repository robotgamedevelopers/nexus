// The byte encoding of nexus's diffs: unsigned varints (LEB128) and length-prefixed byte strings. mosh's transport
// carries a diff as an opaque string (it compresses, encrypts and fragments it); this is what is inside.

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace nexus::sync {

// A diff that cannot be applied: malformed bytes, or a diff that does not start from the state it is applied to. mosh
// authenticates every packet, so this is a bug in the sender, never noise on the wire; the receiver reports it with
// this message and changes nothing.
struct BadDiff : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Writer {
public:
    void u64(std::uint64_t v);
    void byte(std::uint8_t v);
    void str(const std::string& s);
    void raw(const char* data, std::size_t size);
    std::string take() { return std::move(out_); }
    std::size_t size() const { return out_.size(); }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(const std::string& in) : in_(in) {}
    std::uint64_t u64();
    std::uint8_t byte();
    std::string str();
    bool done() const { return at_ == in_.size(); }
    // Throws BadDiff unless every byte was read.
    void end() const;

private:
    const std::string& in_;
    std::size_t at_ = 0;
};

} // namespace nexus::sync
