// Exercise the real server-side incoming tree, including retry after staging and symlinked workspaces.
#include "server/bus_attachment.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

void run(const fs::path& workspace) {
    BusBody out;
    std::string why;
    // Count bytes at the exact boundary, including multibyte UTF-8 and trailing newlines.
    std::string text(4093, 'a');
    text += "中";
    CHECK(prepareBusBody({}, "friend", "124-0", text, out, why));
    CHECK(out.displayBody == text && out.promptBody == text);
    CHECK(!fs::exists(workspace / "incoming"));
    text += '\n';
    const auto deliver = [&](const fs::path& dir, const std::string& who, const std::string& id) {
        return prepareBusBody(dir.string(), who, id, text, out, why);
    };
    CHECK(deliver(workspace, "friend", "124-0"));
    if (out.promptBody.empty()) return;
    const auto first = out.promptBody;
    CHECK(out.displayBody == "[message.txt (4097 bytes)]");
    CHECK(first.starts_with("incoming/") && fs::path(first).filename() == "message.txt");
    CHECK(readFile(workspace / first) == text);
    struct stat st{};
    CHECK(::stat((workspace / first).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(deliver(workspace, "friend", "124-0") && out.promptBody == first);
    CHECK(deliver(workspace, "other", "124-0") && out.promptBody != first);
    CHECK(deliver(workspace, "friend", "124-1") && out.promptBody != first);
    // A retry reconciles a partial left behind by an interrupted write and never consumes modified bytes.
    const auto partial = (workspace / first).parent_path() / ".message.partial";
    fs::remove(workspace / first);
    std::ofstream(partial) << "partial";
    CHECK(deliver(workspace, "friend", "124-0") && out.promptBody == first);
    CHECK(!fs::exists(partial) && readFile(workspace / first) == text);
    std::ofstream(workspace / first) << "a file modified by its owner";
    CHECK(!deliver(workspace, "friend", "124-0") && !why.empty() && out.promptBody.empty());
    CHECK(readFile(workspace / first) == "a file modified by its owner");
    const auto outside = workspace.parent_path() / "outside";
    fs::create_directory(outside);
    const auto targets = workspace.parent_path() / "link-workspace";
    fs::create_directory_symlink(outside, targets);
    CHECK(!deliver(targets, "friend", "123-0") && !why.empty());
    CHECK(!deliver(workspace / ".." / "outside", "friend", "123-0"));
    const auto other = workspace.parent_path() / "other";
    fs::create_directory(other);
    fs::create_directory_symlink(outside, other / "incoming");
    CHECK(!deliver(other, "friend", "123-0"));
    fs::remove(workspace / first);
    fs::create_symlink(outside / "absent.txt", workspace / first);
    CHECK(!deliver(workspace, "friend", "124-0"));
    CHECK(fs::is_empty(outside));
    CHECK(!prepareBusBody(workspace.string(), "friend", "124-2", std::string(kBusAttachmentBytes + 1, 'a'), out, why));
}
} // namespace

int main() {
    auto pattern = (fs::temp_directory_path() / "nexus-bus-attachment-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) return 1;
    const auto temp = fs::canonical(pattern);
    const auto workspace = temp / "workspace";
    fs::create_directory(workspace);
    run(workspace);
    fs::remove_all(temp);
    return failures == 0 ? 0 : 1;
}
