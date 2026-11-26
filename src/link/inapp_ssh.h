// The link's ssh inside the app, for iOS and iPadOS, where an app cannot run /usr/bin/ssh (build_parts.h): a
// CommandRunner (command.h) that answers the two commands the link runs through ssh (link/bootstrap.h), over libssh2
// and mbedTLS (cmake/ios_link.cmake) in this process:
//
// - `ssh -G <host>` (resolveHost): ssh's own config is the Mac's, and the app has none, so it prints only
//   `hostname <host>`, the host as the client config names it, without its user.
// - `ssh -T -o ... <user@host> -- <line>` (bootstrapCommand, and the history pages): connects to port 22, checks the
//   server's host key, signs in with this device's key, runs <line>, and gives back what it printed and its exit status.
//
// The device's key is an ECDSA P-256 key the app makes on first use and keeps in its client home (`ssh-key`, mode 0600,
// never copied anywhere); its public line is `ssh-key.pub` there, and a copy in the app's Documents, which the Files app
// and a Mac's Finder show, so that the person can add it to the server's ~/.ssh/authorized_keys. The server's host key
// is trusted on first use, as ssh asks a person to, and kept in `known_hosts` in the client home; a host key that
// differs from the one kept is refused, with what to do.
//
// The parsing, the key's public line and the known-hosts decision are pure, for the unit tests (tests/ios/ssh_unit.cpp);
// the runner itself dials, and is exercised against a real sshd by the functional test.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "command.h"

namespace nexus::link {

// One command of the link's, read from the arguments bootstrap.h writes.
struct SshCall {
    enum class Kind { Resolve, Run };
    Kind kind = Kind::Run;
    std::string user;   // with Run: before the @; empty if the host names none
    std::string host;   // the host as given, without its user
    std::string line;   // with Run: the remote line
};

// True with the call `c` makes: `-G <host>`, or a host followed by `--` and one remote line (the options before the
// host are the link's own, BatchMode and ConnectTimeout, which the runner keeps anyway). False, with why, for anything
// else, which the runner refuses rather than guesses at.
bool parseSshCall(const Command& c, SshCall* out, std::string* why);

// The authorized_keys line of an ECDSA P-256 public key, given as its 65-byte uncompressed point:
// `ecdsa-sha2-nistp256 <base64 of the ssh wire blob> <comment>`. Empty if the point is not 65 bytes starting 0x04.
std::string ecdsaPublicLine(const std::vector<std::uint8_t>& point, const std::string& comment);

// What the kept known hosts (`text`, one `<host> <type> <base64>` line each, as OpenSSH's known_hosts writes an
// unhashed one) say of the key `type` `base64` that `host` showed.
enum class HostKeyCheck { Known, New, Changed };
HostKeyCheck checkHostKey(const std::string& text, const std::string& host, const std::string& type,
                          const std::string& base64);
std::string knownHostLine(const std::string& host, const std::string& type, const std::string& base64);

// The device's key, made in `dir` (the client home) on first use. `privatePem` is SEC1 PEM; `publicLine` its
// authorized_keys line. False, with why, if it can neither be read nor made.
struct DeviceKey {
    std::string privatePem;
    std::string publicLine;
};
bool deviceKey(const std::string& dir, const std::string& comment, DeviceKey* out, std::string* why);

struct InAppSshOptions {
    std::string dir;            // the client home: the device's key and known_hosts
    std::string publicCopy;     // where a copy of the public line goes for the person (Documents); empty: none
    std::string comment = "nexus-ios";
    int port = 22;
};

// The runner. Each call is its own connection; calls may run on several threads at once.
CommandRunner inAppSsh(InAppSshOptions options);

} // namespace nexus::link
