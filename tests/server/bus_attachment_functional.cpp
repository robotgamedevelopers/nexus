// Exercise the real server-side incoming tree, including retry after staging and symlinked workspaces.
#include "server/bus_attachment.h"
#include "server/bus_listener.h"
#include "server/sha256.h"
#include "store/codec.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace nexus::server;

namespace {
int failures = 0;
#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); ++failures; } } while (false)

std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void filtered(const fs::path& server) {
    fs::create_directory(server);
    const std::string address = "127.0.0.1:6379", recipient = "ai";
    const fs::path archive = server / "bus-filtered";
    BusMessage msg{"999-1", "writer", "990-0", "a subject\nwith a second line", "short"};
    const auto pathFor = [&](const std::string& endpoint, const std::string& inbox) {
        return archive / sha256Hex(endpoint + '\0' + inbox + '\0' + msg.id) / "envelope.bin";
    };
    const auto retain = [&](const std::string& endpoint, const std::string& inbox, std::string& why) {
        // Each call opens a fresh server-state handle to cover process restart before the ACK.
        const int fd = ::open(server.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool ok = retainFilteredBus(fd, endpoint, inbox, msg, why);
        ::close(fd);
        return ok;
    };
    const auto checkEnvelope = [&](const fs::path& name, const std::string& endpoint, const std::string& inbox) {
        const auto bytes = readFile(name);
        CHECK(bytes.starts_with("NBUS\1"));
        if (bytes.size() < 5) return;
        nexus::store::Reader reader({reinterpret_cast<const std::uint8_t*>(bytes.data() + 5), bytes.size() - 5});
        CHECK(reader.str() == endpoint && reader.str() == inbox && reader.str() == msg.id && reader.str() == msg.from);
        CHECK(reader.str() == msg.replyTo && reader.str() == msg.subject && reader.str() == msg.body);
        CHECK(reader.ok() && reader.done());
        struct stat st{};
        CHECK(::stat(archive.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
        CHECK(::stat(name.parent_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
        CHECK(::stat(name.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    };
    std::string why;
    fs::create_directory(archive);
    fs::permissions(archive, fs::perms::owner_all | fs::perms::group_read);
    CHECK(!retain(address, recipient, why) && !why.empty() && fs::is_empty(archive));
    fs::permissions(archive, fs::perms::owner_all);
    CHECK(retain(address, recipient, why));
    const auto first = pathFor(address, recipient);
    checkEnvelope(first, address, recipient);
    struct stat before{}, after{};
    CHECK(::stat(first.c_str(), &before) == 0);
    CHECK(retain(address, recipient, why));
    CHECK(::stat(first.c_str(), &after) == 0 && before.st_ino == after.st_ino);
    CHECK(std::distance(fs::directory_iterator(archive), fs::directory_iterator()) == 1);
    msg.body = std::string(8192, 'z') + std::string("\0\n\t", 3);
    CHECK(!retain(address, recipient, why) && !why.empty()); // an immutable receipt cannot replace earlier mail
    const std::string other = "127.0.0.1:6380";
    CHECK(retain(other, recipient, why));
    checkEnvelope(pathFor(other, recipient), other, recipient);
    CHECK(retain(address, "other", why));
    checkEnvelope(pathFor(address, "other"), address, "other");
    fs::permissions(first, fs::perms::owner_read | fs::perms::owner_write | fs::perms::others_read);
    msg.body = "short";
    CHECK(!retain(address, recipient, why) && !why.empty());
}

void run(const fs::path& workspace) {
    BusBody out;
    std::string why;
    // Count bytes at the exact boundary, including multibyte UTF-8 and trailing newlines.
    std::string text(4093, 'a');
    text += "中";
    CHECK(prepareBusBody({}, "ai", "124-0", text, out, why));
    CHECK(out.displayBody == text && out.promptBody == text);
    CHECK(!fs::exists(workspace / "incoming"));
    text += '\n';
    const auto deliver = [&](const fs::path& dir, const std::string& who, const std::string& id) {
        return prepareBusBody(dir.string(), who, id, text, out, why);
    };
    CHECK(deliver(workspace, "ai", "124-0"));
    if (out.promptBody.empty()) return;
    const auto first = out.promptBody;
    CHECK(out.displayBody == "[message.txt (4097 bytes)]");
    CHECK(first.starts_with("incoming/") && fs::path(first).filename() == "message.txt");
    CHECK(readFile(workspace / first) == text);
    struct stat st{};
    CHECK(::stat((workspace / first).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(deliver(workspace, "ai", "124-0") && out.promptBody == first);
    CHECK(deliver(workspace, "other", "124-0") && out.promptBody != first);
    CHECK(deliver(workspace, "ai", "124-1") && out.promptBody != first);
    // A retry reconciles a partial left behind by an interrupted write and never consumes modified bytes.
    const auto partial = (workspace / first).parent_path() / ".message.partial";
    fs::remove(workspace / first);
    std::ofstream(partial) << "partial";
    CHECK(deliver(workspace, "ai", "124-0") && out.promptBody == first);
    CHECK(!fs::exists(partial) && readFile(workspace / first) == text);
    std::ofstream(workspace / first) << "a file modified by its owner";
    CHECK(!deliver(workspace, "ai", "124-0") && !why.empty() && out.promptBody.empty());
    CHECK(readFile(workspace / first) == "a file modified by its owner");
    const auto outside = workspace.parent_path() / "outside";
    fs::create_directory(outside);
    const auto targets = workspace.parent_path() / "link-workspace";
    fs::create_directory_symlink(outside, targets);
    CHECK(!deliver(targets, "ai", "123-0") && !why.empty());
    CHECK(!deliver(workspace / ".." / "outside", "ai", "123-0"));
    const auto other = workspace.parent_path() / "other";
    fs::create_directory(other);
    fs::create_directory_symlink(outside, other / "incoming");
    CHECK(!deliver(other, "ai", "123-0"));
    fs::remove(workspace / first);
    fs::create_symlink(outside / "absent.txt", workspace / first);
    CHECK(!deliver(workspace, "ai", "124-0"));
    CHECK(fs::is_empty(outside));
    CHECK(!prepareBusBody(workspace.string(), "ai", "124-2", std::string(kBusAttachmentBytes + 1, 'a'), out, why));
}
} // namespace

int main() {
    auto pattern = (fs::temp_directory_path() / "nexus-bus-attachment-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) return 1;
    const auto temp = fs::canonical(pattern);
    const auto workspace = temp / "workspace";
    fs::create_directory(workspace);
    run(workspace);
    filtered(temp / "server");
    fs::remove_all(temp);
    return failures == 0 ? 0 : 1;
}
