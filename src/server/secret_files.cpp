#include "server/secret_files.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace nexus::server {

namespace {

const char* const kTempSuffix = ".new";

bool endsWith(const std::string& s, const std::string& end) {
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

} // namespace

bool isPathPart(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
        std::none_of(name.begin(), name.end(), [](char c) { return c == '/' || static_cast<unsigned char>(c) < 0x20; });
}

std::string AccountFiles::path(const std::string& account, const std::string& what) const {
    return accounts_ + "/" + account + "/" + what;
}

sync::Kept AccountFiles::keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
                              std::string* why) {
    auto fail = [&](const std::string& text) {
        if (why != nullptr) {
            *why = text;
        }
        return sync::Kept::Failed;
    };
    if (!isPathPart(account) || !isPathPart(what)) {
        return fail("the account or the file is not a name a path can hold");
    }
    const std::string dir = accounts_ + "/" + account;
    const std::string file = dir + "/" + what;
    const std::string temp = file + kTempSuffix;
    // The account's directory is the server's own and no one else's: never a link, never open to other users.
    store::Status s = fs_.private_dir(dir);
    if (!s.is_ok()) {
        return fail(s.message());
    }
    // A case-insensitive filesystem can open another account when the requested spelling is absent.
    std::vector<std::string> names;
    s = fs_.list(accounts_, names);
    if (!s.is_ok()) {
        return fail(s.message());
    }
    if (std::find(names.begin(), names.end(), account) == names.end()) {
        return fail(dir + " does not match the spelling of the account directory; check the account name's capitalization");
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
    // Created, written and synced through one descriptor, never followed through a link or opened again by name.
    s = fs_.write_new(temp, data);
    if (s.is_ok()) {
        s = fs_.rename(temp, file);
    }
    if (!s.is_ok()) {
        fs_.remove(temp);
        return fail(s.message());
    }
    // Renamed: the value is in its file now. A failed sync of the directory leaves its durability unconfirmed, not the
    // value unwritten.
    s = fs_.sync_dir(dir);
    if (!s.is_ok()) {
        if (why != nullptr) {
            *why = "the value is in " + file + ", but the sync of its directory failed, so a crash may bring the old "
                "file back: " + s.message();
        }
        return sync::Kept::Unconfirmed;
    }
    return sync::Kept::Durable;
}

std::vector<std::string> AccountFiles::sweep() {
    std::vector<std::string> report;
    std::vector<std::string> accounts;
    store::Status s = fs_.list(accounts_, accounts);
    if (!s.is_ok()) {
        if (s.code() != store::Status::Code::NotFound) {
            report.push_back("the accounts were not swept for temporary files: " + s.message());
        }
        return report;
    }
    std::sort(accounts.begin(), accounts.end());
    for (const std::string& account : accounts) {
        const std::string dir = accounts_ + "/" + account;
        std::vector<std::string> names;
        if (!isPathPart(account) || !fs_.list(dir, names).is_ok()) {
            continue;   // not a directory (or not one this server can read): nothing of its
        }
        if (s = fs_.private_dir(dir); !s.is_ok()) {
            report.push_back("not swept: " + s.message());
            continue;
        }
        bool removed = false;
        for (const std::string& name : names) {
            if (!endsWith(name, kTempSuffix)) {
                continue;
            }
            s = fs_.remove(dir + "/" + name);
            if (s.is_ok()) {
                removed = true;
                report.push_back("removed " + dir + "/" + name + ", left by a write a crash or a failure cut short");
            } else {
                report.push_back("could not remove " + dir + "/" + name + ": " + s.message());
            }
        }
        if (removed) {
            if (s = fs_.sync_dir(dir); !s.is_ok()) {
                report.push_back(s.message());
            }
        }
    }
    return report;
}

} // namespace nexus::server
