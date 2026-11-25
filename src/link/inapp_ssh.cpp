#include "link/inapp_ssh.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include <libssh2.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>

namespace nexus::link {

namespace {

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::uint8_t* data, std::size_t size) {
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        std::uint32_t n = std::uint32_t(data[i]) << 16;
        if (i + 1 < size) n |= std::uint32_t(data[i + 1]) << 8;
        if (i + 2 < size) n |= data[i + 2];
        out += kBase64[(n >> 18) & 63];
        out += kBase64[(n >> 12) & 63];
        out += i + 1 < size ? kBase64[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kBase64[n & 63] : '=';
    }
    return out;
}

void putString(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t size) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(std::uint8_t(size >> shift));
    out.insert(out.end(), data, data + size);
}

void putString(std::vector<std::uint8_t>& out, const std::string& s) {
    putString(out, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

bool readFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    *out = s.str();
    return true;
}

// Writes `text` to `path` with `mode`, replacing it whole (a temporary file, then rename).
bool writeFile(const std::string& path, const std::string& text, mode_t mode, std::string* why) {
    const std::string temporary = path + ".new";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        *why = "cannot write " + temporary + ": " + std::strerror(errno);
        return false;
    }
    const bool written = write(fd, text.data(), text.size()) == ssize_t(text.size());
    close(fd);
    if (!written || rename(temporary.c_str(), path.c_str()) != 0) {
        *why = "cannot write " + path + ": " + std::strerror(errno);
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

struct Random {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool seeded = false;
    Random() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        static const char personal[] = "nexus-ios ssh key";
        seeded = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
            reinterpret_cast<const unsigned char*>(personal), sizeof personal - 1) == 0;
    }
    ~Random() {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

// The public line of the key in `pem`. False, with why.
bool publicLineOf(const std::string& pem, const std::string& comment, std::string* line, std::string* why) {
    Random random;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    std::vector<std::uint8_t> point(65);
    std::size_t length = 0;
    int code = random.seeded ? 0 : -1;
    if (code == 0)
        code = mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1,
            nullptr, 0, mbedtls_ctr_drbg_random, &random.drbg);
    const bool ec = code == 0 && mbedtls_pk_get_type(&pk) == MBEDTLS_PK_ECKEY;
    if (ec)
        code = mbedtls_ecp_write_public_key(mbedtls_pk_ec(pk), MBEDTLS_ECP_PF_UNCOMPRESSED, &length, point.data(),
            point.size());
    mbedtls_pk_free(&pk);
    if (ec && code == 0 && length == 65) *line = ecdsaPublicLine(point, comment);
    if (!ec || code != 0 || line->empty()) {
        char text[200] = "";
        if (code != 0) mbedtls_strerror(code, text, sizeof text);
        *why = std::string("the device's ssh key does not read as an ECDSA P-256 key") + (text[0] ? ": " : "") + text;
        return false;
    }
    return true;
}

bool makeKey(std::string* pem, std::string* why) {
    Random random;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    std::vector<unsigned char> buffer(1024, 0);
    const bool ok = random.seeded &&
        mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &random.drbg) == 0 &&
        mbedtls_pk_write_key_pem(&pk, buffer.data(), buffer.size()) == 0;
    mbedtls_pk_free(&pk);
    if (!ok) {
        *why = "making the device's ssh key failed";
        return false;
    }
    *pem = reinterpret_cast<const char*>(buffer.data());
    return true;
}

std::mutex& fileLock() {
    static std::mutex m;
    return m;
}

const char* hostKeyType(int type) {
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return "ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ecdsa-sha2-nistp521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ssh-ed25519";
    default: return "unknown";
    }
}

std::string sshError(LIBSSH2_SESSION* session) {
    char* message = nullptr;
    int length = 0;
    libssh2_session_last_error(session, &message, &length, 0);
    return message != nullptr ? std::string(message, std::size_t(length)) : std::string("unknown error");
}

CommandResult failed(int code, std::string err) {
    CommandResult r;
    r.end = CommandResult::End::Exited;
    r.code = code;
    r.err = std::move(err);
    return r;
}

// A TCP connection to host:port within `timeoutMs`; -1, with why.
int dial(const std::string& host, int port, int timeoutMs, std::string* why) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found);
    if (rc != 0 || found == nullptr) {
        *why = "cannot resolve " + host + ": " + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    std::string last = "no address";
    for (const addrinfo* a = found; a != nullptr && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            r = poll(&p, 1, timeoutMs) == 1 ? 0 : -1;
            int error = r == 0 ? 0 : ETIMEDOUT;
            socklen_t size = sizeof error;
            if (r == 0) getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
            if (error != 0) {
                errno = error;
                r = -1;
            }
        }
        if (r != 0) {
            last = std::strerror(errno);
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(found);
    if (fd < 0) {
        *why = "cannot connect to " + host + " port " + std::to_string(port) + ": " + last;
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

CommandResult runOver(const InAppSshOptions& o, const SshCall& call, int timeoutSeconds) {
    static std::once_flag initialized;
    std::call_once(initialized, [] { libssh2_init(0); });
    const std::string where = call.user + "@" + call.host;
    if (call.user.empty())
        return failed(255, "no user in `" + call.host + "`: the server is named `user@host` in the client config "
            "(server user@host), since this device has no ssh config to take the user from");
    DeviceKey key;
    std::string why;
    if (!deviceKey(o.dir, o.comment, &key, &why)) return failed(255, why);
    if (!o.publicCopy.empty()) {
        std::string held;
        if (!readFile(o.publicCopy, &held) || held != key.publicLine + "\n")
            if (!writeFile(o.publicCopy, key.publicLine + "\n", 0644, &why)) std::fprintf(stderr, "nexus: %s\n", why.c_str());
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    const int fd = dial(call.host, o.port, 10000, &why);
    if (fd < 0) return failed(255, why);
    LIBSSH2_SESSION* session = libssh2_session_init();
    LIBSSH2_CHANNEL* channel = nullptr;
    CommandResult result;
    auto finish = [&](CommandResult r) {
        if (channel != nullptr) libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "done");
        libssh2_session_free(session);
        close(fd);
        return r;
    };
    libssh2_session_set_blocking(session, 1);
    libssh2_session_set_timeout(session, long(timeoutSeconds) * 1000);
    if (libssh2_session_handshake(session, fd) != 0)
        return finish(failed(255, "ssh to " + where + ": the handshake failed: " + sshError(session)));

    // The host key, trusted on first use.
    std::size_t keyLength = 0;
    int keyType = 0;
    const char* hostKey = libssh2_session_hostkey(session, &keyLength, &keyType);
    if (hostKey == nullptr)
        return finish(failed(255, "ssh to " + where + ": the server showed no host key"));
    const std::string type = hostKeyType(keyType);
    const std::string encoded = base64(reinterpret_cast<const std::uint8_t*>(hostKey), keyLength);
    // As OpenSSH names it in known_hosts: the host, or [host]:port off port 22.
    const std::string hostId = o.port == 22 ? call.host : "[" + call.host + "]:" + std::to_string(o.port);
    {
        std::lock_guard<std::mutex> lock(fileLock());
        const std::string knownPath = o.dir + "/known_hosts";
        std::string known;
        readFile(knownPath, &known);
        switch (checkHostKey(known, hostId, type, encoded)) {
        case HostKeyCheck::Known:
            break;
        case HostKeyCheck::New:
            if (!known.empty() && known.back() != '\n') known += '\n';
            if (!writeFile(knownPath, known + knownHostLine(hostId, type, encoded) + "\n", 0600, &why))
                return finish(failed(255, why));
            break;
        case HostKeyCheck::Changed:
            return finish(failed(255, "ssh to " + where + ": the server's " + type + " host key is not the one kept "
                "for it in " + knownPath + ". If its keys were made again, remove its line there; otherwise "
                "something stands between this device and the server"));
        }
    }

    if (libssh2_userauth_publickey_frommemory(session, call.user.c_str(), call.user.size(), key.publicLine.c_str(),
            key.publicLine.size(), key.privatePem.c_str(), key.privatePem.size(), nullptr) != 0)
        return finish(failed(255, "ssh to " + where + ": the server did not take this device's key (" +
            sshError(session) + "). Add this line to " + call.user + "'s ~/.ssh/authorized_keys on " + call.host +
            " (it is also in this app's Documents, nexus-ssh-key.pub): " + key.publicLine));

    channel = libssh2_channel_open_session(session);
    if (channel == nullptr)
        return finish(failed(255, "ssh to " + where + ": no session channel: " + sshError(session)));
    if (libssh2_channel_exec(channel, call.line.c_str()) != 0)
        return finish(failed(255, "ssh to " + where + ": the command did not start: " + sshError(session)));

    // Both streams until the end, without blocking on one while the other fills.
    libssh2_session_set_blocking(session, 0);
    char buffer[16384];
    for (;;) {
        bool moved = false;
        ssize_t n;
        while ((n = libssh2_channel_read(channel, buffer, sizeof buffer)) > 0) {
            result.out.append(buffer, std::size_t(n));
            moved = true;
        }
        const ssize_t outEnd = n;
        while ((n = libssh2_channel_read_stderr(channel, buffer, sizeof buffer)) > 0) {
            result.err.append(buffer, std::size_t(n));
            moved = true;
        }
        if ((outEnd < 0 && outEnd != LIBSSH2_ERROR_EAGAIN) || (n < 0 && n != LIBSSH2_ERROR_EAGAIN))
            return finish(failed(255, "ssh to " + where + ": reading the command's output failed: " +
                sshError(session)));
        if (libssh2_channel_eof(channel)) break;
        if (moved) continue;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
            std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            result.end = CommandResult::End::TimedOut;
            return finish(result);
        }
        const int directions = libssh2_session_block_directions(session);
        pollfd p{fd, short(((directions & LIBSSH2_SESSION_BLOCK_INBOUND) ? POLLIN : 0) |
            ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? POLLOUT : 0)), 0};
        if (p.events == 0) p.events = POLLIN;
        poll(&p, 1, int(std::min<long long>(left, 1000)));
    }
    while (libssh2_channel_close(channel) == LIBSSH2_ERROR_EAGAIN) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 1000) <= 0) break;
    }
    char* signal = nullptr;
    libssh2_channel_get_exit_signal(channel, &signal, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (signal != nullptr) {
        result.end = CommandResult::End::Signaled;
        libssh2_free(session, signal);
    } else {
        result.end = CommandResult::End::Exited;
        result.code = libssh2_channel_get_exit_status(channel);
    }
    return finish(result);
}

} // namespace

bool parseSshCall(const Command& c, SshCall* out, std::string* why) {
    const auto& a = c.arguments;
    SshCall call;
    if (a.size() == 2 && a[0] == "-G") {
        call.kind = SshCall::Kind::Resolve;
        call.host = a[1];
    } else {
        std::size_t dashes = 0;
        while (dashes < a.size() && a[dashes] != "--") ++dashes;
        if (dashes == 0 || dashes + 2 != a.size() || a[dashes - 1].empty() || a[dashes - 1][0] == '-') {
            *why = "the in-app ssh runs only the link's own commands, not `" + c.shown + "`";
            return false;
        }
        call.kind = SshCall::Kind::Run;
        call.host = a[dashes - 1];
        call.line = a[dashes + 1];
    }
    const auto at = call.host.rfind('@');
    if (at != std::string::npos) {
        call.user = call.host.substr(0, at);
        call.host = call.host.substr(at + 1);
    }
    if (call.host.empty()) {
        *why = "no host in `" + c.shown + "`";
        return false;
    }
    *out = call;
    return true;
}

std::string ecdsaPublicLine(const std::vector<std::uint8_t>& point, const std::string& comment) {
    if (point.size() != 65 || point[0] != 0x04) return {};
    std::vector<std::uint8_t> blob;
    putString(blob, "ecdsa-sha2-nistp256");
    putString(blob, "nistp256");
    putString(blob, point.data(), point.size());
    std::string line = "ecdsa-sha2-nistp256 " + base64(blob.data(), blob.size());
    if (!comment.empty()) line += " " + comment;
    return line;
}

HostKeyCheck checkHostKey(const std::string& text, const std::string& host, const std::string& type,
                          const std::string& base64Key) {
    std::istringstream lines(text);
    std::string line;
    bool seen = false;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string hosts, keptType, kept;
        if (!(words >> hosts >> keptType >> kept) || hosts[0] == '#') continue;
        bool named = false;
        std::istringstream names(hosts);
        std::string name;
        while (std::getline(names, name, ',')) named = named || name == host;
        if (!named || keptType != type) continue;
        if (kept == base64Key) return HostKeyCheck::Known;
        seen = true;
    }
    return seen ? HostKeyCheck::Changed : HostKeyCheck::New;
}

std::string knownHostLine(const std::string& host, const std::string& type, const std::string& base64Key) {
    return host + " " + type + " " + base64Key;
}

bool deviceKey(const std::string& dir, const std::string& comment, DeviceKey* out, std::string* why) {
    std::lock_guard<std::mutex> lock(fileLock());
    const std::string path = dir + "/ssh-key";
    DeviceKey key;
    if (!readFile(path, &key.privatePem)) {
        if (!makeKey(&key.privatePem, why) || !writeFile(path, key.privatePem, 0600, why)) return false;
    }
    if (!publicLineOf(key.privatePem, comment, &key.publicLine, why)) {
        *why += " (" + path + ")";
        return false;
    }
    std::string held;
    if (!readFile(path + ".pub", &held) || held != key.publicLine + "\n")
        if (std::string copy; !writeFile(path + ".pub", key.publicLine + "\n", 0644, &copy)) std::fprintf(stderr, "nexus: %s\n", copy.c_str());
    *out = std::move(key);
    return true;
}

CommandRunner inAppSsh(InAppSshOptions options) {
    return [options](const Command& c, int timeoutSeconds) {
        SshCall call;
        std::string why;
        if (!parseSshCall(c, &call, &why)) {
            CommandResult r;
            r.end = CommandResult::End::NotStarted;
            r.err = why;
            return r;
        }
        if (call.kind == SshCall::Kind::Resolve) {
            CommandResult r;
            r.end = CommandResult::End::Exited;
            r.out = "hostname " + call.host + "\n";
            return r;
        }
        return runOver(options, call, timeoutSeconds);
    };
}

} // namespace nexus::link
