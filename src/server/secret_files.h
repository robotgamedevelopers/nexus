// Where the secret dialog's values go (docs/secrets.md): `accounts/<name>/<what>` under the server's accounts directory
// (server/home.h accountsDir), the 0700 directory a harness login lives in. A value is written to a temporary 0600 file
// beside its file, synced, renamed over it, and the directory synced, all before keep() returns: so the asker's
// "stored", which the hub gives only once keep() has returned true, goes out only once the file is durable (and the
// server's loop sends nothing of a pass before its flush, server/pass.h). Through store::Fs, so the unit tests write
// to MemFs and crash it at every call.

#pragma once

#include <string>

#include "store/fs.h"
#include "sync/secrets.h"

namespace nexus::server {

class AccountFiles : public sync::SecretKeeper {
public:
    AccountFiles(store::Fs& fs, std::string accounts) : fs_(fs), accounts_(std::move(accounts)) {}

    // False, with why (a path and the system's error, never the value), when the file could not be written: then the
    // old file, if any, is as it was.
    bool keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
              std::string* why) override;

    // The file a value for `account`'s `what` goes to.
    std::string path(const std::string& account, const std::string& what) const;

private:
    store::Fs& fs_;
    std::string accounts_;
};

// Whether `name` may be one part of a path under the accounts directory: not empty, not "." or "..", no '/' and no
// control character.
bool isPathPart(const std::string& name);

} // namespace nexus::server
