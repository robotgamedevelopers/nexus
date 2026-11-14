#include "server/secret_files.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace nexus::server {

bool isPathPart(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
        std::none_of(name.begin(), name.end(), [](char c) { return c == '/' || static_cast<unsigned char>(c) < 0x20; });
}

std::string AccountFiles::path(const std::string& account, const std::string& what) const {
    return accounts_ + "/" + account + "/" + what;
}

bool AccountFiles::keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
                        std::string* why) {
    auto fail = [&](const std::string& text) {
        if (why != nullptr) {
            *why = text;
        }
        return false;
    };
    if (!isPathPart(account) || !isPathPart(what)) {
        return fail("the account or the file is not a name a path can hold");
    }
    const std::string dir = accounts_ + "/" + account;
    const std::string file = dir + "/" + what;
    const std::string temp = file + ".new";
    store::Status s = fs_.make_dir(dir);
    if (!s.is_ok()) {
        return fail(s.message());
    }
    // A temporary file left by a write a crash cut short holds nothing anyone reads: it goes first.
    s = fs_.remove(temp);
    if (!s.is_ok() && s.code() != store::Status::Code::NotFound) {
        std::uint64_t size = 0;
        if (fs_.size(temp, size).is_ok()) {
            return fail(s.message());
        }
    }
    const std::string& bytes = value.bytes();
    const std::span<const std::uint8_t> data(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    s = fs_.create(temp);
    if (s.is_ok()) {
        s = fs_.append(temp, data);
    }
    if (s.is_ok()) {
        s = fs_.sync(temp);
    }
    if (s.is_ok()) {
        s = fs_.rename(temp, file);
    }
    if (s.is_ok()) {
        s = fs_.sync_dir(dir);
    }
    if (!s.is_ok()) {
        fs_.remove(temp);
        return fail(s.message());
    }
    return true;
}

} // namespace nexus::server
