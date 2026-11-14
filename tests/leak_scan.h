// How the tests find a fake secret value in what nexus wrote somewhere (docs/secrets.md): the value, any run of 8 or
// more of its bytes, its hex (either case, any run of 8 or more of its bytes), its base64 (standard and URL-safe, at
// each of the three alignments, any run of 9 or more of its bytes), and its length written as a number within 64 bytes
// of the account's name. A test makes its value up, of letters found in nothing else it writes, so a match is a leak
// and never chance. Header only: every test that scans links nothing more.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nexus::test {

class LeakScan {
public:
    // `account`: the account the value is for, whose name a printed length would sit beside; empty for none.
    explicit LeakScan(std::string value, std::string account = std::string())
        : value_(std::move(value)), account_(std::move(account)) {
        constexpr std::size_t kRun = 8;
        for (std::size_t i = 0; i + kRun <= value_.size(); ++i) {
            add(value_.substr(i, kRun), "a run of its bytes");
        }
        if (value_.size() < kRun && !value_.empty()) {
            add(value_, "the value");
        }
        for (const bool upper : {false, true}) {
            const std::string h = hex(value_, upper);
            for (std::size_t i = 0; i + 2 * kRun <= h.size(); i += 2) {
                add(h.substr(i, 2 * kRun), "its hex");
            }
        }
        for (std::size_t shift = 0; shift < 3 && shift < value_.size(); ++shift) {
            for (const bool url : {false, true}) {
                const std::string b = base64(value_.substr(shift), url);
                // The quads before the last, whose characters each depend on the value's bytes alone.
                const std::size_t body = b.size() >= 4 ? b.size() - 4 : 0;
                for (std::size_t i = 0; i + 12 <= body; i += 4) {
                    add(b.substr(i, 12), "its base64");
                }
            }
        }
    }

    // What in `text` gives the value away ("its hex at 120"), or empty when nothing does.
    std::string find(std::string_view text) const {
        for (std::size_t n = 0; n < needles_.size(); ++n) {
            const std::size_t at = text.find(needles_[n]);
            if (at != std::string_view::npos) {
                return kinds_[n] + " at byte " + std::to_string(at);
            }
        }
        if (!account_.empty()) {
            const std::string length = std::to_string(value_.size());
            for (std::size_t at = text.find(length); at != std::string_view::npos; at = text.find(length, at + 1)) {
                const bool alone = (at == 0 || !digit(text[at - 1])) &&
                    (at + length.size() == text.size() || !digit(text[at + length.size()]));
                if (!alone) {
                    continue;
                }
                const std::size_t from = at >= 64 + account_.size() ? at - 64 - account_.size() : 0;
                const std::string_view near = text.substr(from, at - from + length.size() + 64 + account_.size());
                if (near.find(account_) != std::string_view::npos) {
                    return "its length (" + length + ") beside the account " + account_ + " at byte " +
                           std::to_string(at);
                }
            }
        }
        return std::string();
    }

    bool in(std::string_view text) const { return !find(text).empty(); }
    const std::string& value() const { return value_; }

    static std::string hex(std::string_view bytes, bool upper) {
        const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        std::string out;
        for (const char c : bytes) {
            const unsigned char b = static_cast<unsigned char>(c);
            out += digits[b >> 4];
            out += digits[b & 15];
        }
        return out;
    }

    static std::string base64(std::string_view bytes, bool url) {
        const char* table = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                                : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        std::size_t i = 0;
        for (; i + 3 <= bytes.size(); i += 3) {
            const std::uint32_t n = (std::uint32_t(static_cast<unsigned char>(bytes[i])) << 16) |
                (std::uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                std::uint32_t(static_cast<unsigned char>(bytes[i + 2]));
            out += table[(n >> 18) & 63];
            out += table[(n >> 12) & 63];
            out += table[(n >> 6) & 63];
            out += table[n & 63];
        }
        if (i < bytes.size()) {
            std::uint32_t n = std::uint32_t(static_cast<unsigned char>(bytes[i])) << 16;
            if (i + 1 < bytes.size()) {
                n |= std::uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8;
            }
            out += table[(n >> 18) & 63];
            out += table[(n >> 12) & 63];
            out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

private:
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    void add(std::string needle, const char* kind) {
        needles_.push_back(std::move(needle));
        kinds_.emplace_back(kind);
    }

    std::string value_;
    std::string account_;
    std::vector<std::string> needles_;
    std::vector<std::string> kinds_;
};

} // namespace nexus::test
