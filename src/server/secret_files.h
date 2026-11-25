// Where the secret dialog's values go (docs/secrets.md): `accounts/<name>/<what>` under the server's accounts directory
// (server/home.h accountsDir), the 0700 directory a harness login lives in. A value is written to a temporary 0600 file
// beside its file, created, written and synced through one descriptor (never through a link), renamed over it, and
// the directory synced, all before keep() returns: so the asker's plain "stored", which the hub gives only once keep()
// has returned Durable, goes out only once the file is durable (and the server's loop sends nothing of a pass before
// its flush, server/pass.h). Through store::Fs, so the unit tests write to MemFs and crash it at every call.

#pragma once

#include <string>
#include <vector>

#include "store/fs.h"
#include "sync/secrets.h"

namespace nexus::server {

class AccountFiles : public sync::SecretKeeper {
public:
    AccountFiles(store::Fs& fs, std::string accounts) : fs_(fs), accounts_(std::move(accounts)) {}

    // Failed, with why (a path and the system's error, never the value), when the file could not be written: then the
    // old file, if any, is as it was. Failed too when the account's directory is a symbolic link, not the server's
    // user's or open to other users. Unconfirmed, with why, when the value is in its file but the directory's sync
    // failed after the rename.
    sync::Kept keep(const std::string& account, const std::string& what, const sync::SecretValue& value,
                    std::string* why) override;

    // The file a value for `account`'s `what` goes to.
    std::string path(const std::string& account, const std::string& what) const;

    // At the server's start: every temporary file (`<what>.new`) a write left in an account's directory is removed,
    // and that directory synced. Each line of the report says what was removed, or why it was not; an account
    // directory that is not private is reported and left as it is.
    std::vector<std::string> sweep();
    // Remove both Claude login formats for precisely this account. Other files, including the lock, survive.
    std::string forgetClaude(const std::string& account);

private:
    store::Fs& fs_;
    std::string accounts_;
};

// Whether `name` may be one part of a path under the accounts directory: not empty, not "." or "..", no '/' and no
// control character.
bool isPathPart(const std::string& name);

} // namespace nexus::server
