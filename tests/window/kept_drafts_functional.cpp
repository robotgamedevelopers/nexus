// Functional tests of the kept drafts' file (tab_order.h KeptDraftsStore, fileKeptDraftsStore): the real file and its
// lock, in a directory of the test's own, with windows as main.cpp makes them (the drafts for the window's server read
// at launch into its AIStrip, each change of a take handed to the store through a StoreWriter).
//
//   nexus_kept_drafts_functional <dir> <case>
//
// The cases: two_windows (two windows of one server keep drafts, one of them for the same AI, and neither loses the
// other's; a third gets both back, and the window that gave back only its own leaves the rest), two_servers (a draft
// kept on one server is not given to an AI of the same name on another), and racing (processes changing the file at
// once, each through the lock: every draft each kept is there at the end). Each checks that the file and its lock are
// 0600.

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "model.h"
#include "sync/client.h"
#include "sync/ai_strip.h"
#include "sync/sessions.h"
#include "tab_order.h"

namespace {

using namespace nexus;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

sync::Sessions list(const std::vector<std::string>& names) {
    sync::Sessions s;
    std::vector<sync::ServerAI> ais;
    for (const std::string& n : names) {
        ais.push_back(sync::ServerAI{n});
    }
    s.setAIs(std::move(ais));
    return s;
}

// One window linked to `server`, as main.cpp and FrameHost::takeAIs make it.
struct Window {
    std::string server;
    StoreWriter writer;
    KeptDraftsStore store;
    Model model{{}};
    sync::AIStrip strip;

    Window(const std::string& path, std::string on) : server(std::move(on)) {
        store = writer.wrap(fileKeptDraftsStore(path));
        bool readable = false;
        std::string warning;
        sync::KeptDrafts kept = loadKeptDrafts(store, server, &readable, &warning);
        CHECK(readable && warning.empty());
        strip = sync::AIStrip({}, std::string(), std::move(kept));
    }

    void take(const std::vector<std::string>& names) {
        sync::StripTake t = strip.take(model, list(names));
        if (t.keptChanged) {
            std::string error;
            CHECK(saveKeptChange(store, KeptChange{server, std::move(t.put), std::move(t.taken)}, &error));
        }
        writer.flush();
    }

    std::size_t tabOf(const std::string& name) const {
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            if (model.tab(i).name == name) {
                return i;
            }
        }
        return model.tab_count();
    }

    void type(const std::string& name, const std::string& text) { CHECK(model.set_draft(tabOf(name), text)); }

    std::string draft(const std::string& name) const {
        const std::size_t i = tabOf(name);
        return i < model.tab_count() ? model.tab(i).draft : std::string("(no tab)");
    }
};

// The file as it is now, read without the lock (a rename never shows half of one).
KeptFile fileOf(const std::string& path) {
    KeptFile file;
    std::string error;
    CHECK(fileKeptDraftsStore(path).read(&file, &error));
    return file;
}

// The draft `file` keeps for `server` and `name`, or "(none)".
std::string kept(const KeptFile& file, const std::string& server, const std::string& name) {
    const auto at = file.find({server, name});
    return at == file.end() ? std::string("(none)") : at->second;
}

int modeOf(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 ? static_cast<int>(st.st_mode & 0777) : -1;
}

void privateFiles(const std::string& dir) {
    CHECK(modeOf(dir + "/kept-drafts.txt") == 0600);
    CHECK(modeOf(dir + "/kept-drafts.lock") == 0600);
}

void twoWindows(const std::string& dir) {
    const std::string path = dir + "/kept-drafts.txt";
    Window a(path, "studio");
    Window b(path, "studio");
    a.take({"ada", "bo", "cy"});
    b.take({"ada", "bo", "cy"});
    a.type("bo", "A typed to bo");
    b.type("cy", "B typed to cy");
    b.type("bo", "B typed to bo");
    a.take({"ada"});
    KeptFile file = fileOf(path);
    CHECK(file.size() == 1 && kept(file, "studio", "bo") == "A typed to bo");
    // B's change is merged into the file A changed: A's draft for bo is joined, not written over.
    b.take({"ada"});
    file = fileOf(path);
    CHECK(file.size() == 2);
    CHECK(kept(file, "studio", "bo") == "A typed to bo\n\nB typed to bo");
    CHECK(kept(file, "studio", "cy") == "B typed to cy");
    privateFiles(dir);
    // A third window, launched now, gets both back when the AIs return, and gives them back to the file's no more.
    Window c(path, "studio");
    c.take({"ada", "bo", "cy"});
    CHECK(c.draft("bo") == "A typed to bo\n\nB typed to bo" && c.draft("cy") == "B typed to cy");
    CHECK(fileOf(path).empty());
    // B, which kept only its own, gives back what it holds, and the file is not changed by it.
    b.take({"ada", "bo", "cy"});
    CHECK(b.draft("bo") == "B typed to bo" && b.draft("cy") == "B typed to cy");
    CHECK(fileOf(path).empty());
    // A draft kept while another window holds an older one for the same AI: the older one, given back, takes only
    // itself (the file then holds the newer).
    a.type("ada", "A to ada");
    a.take({});
    Window d(path, "studio");      // reads "A to ada"
    b.type("ada", "B to ada");
    b.take({});
    CHECK(kept(fileOf(path), "studio", "ada") == "A to ada\n\nB to ada");
    d.take({"ada"});
    CHECK(d.draft("ada") == "A to ada");
    CHECK(kept(fileOf(path), "studio", "ada") == "A to ada\n\nB to ada");
    privateFiles(dir);
}

void twoServers(const std::string& dir) {
    const std::string path = dir + "/kept-drafts.txt";
    {
        Window studio(path, "studio");
        studio.take({"ada", "bo"});
        studio.type("ada", "for studio's ada");
        studio.take({"bo"});
    }
    // A window of another server with an AI of the same name: nothing of studio's is given to it, or taken.
    {
        Window mini(path, "me@mini");
        mini.take({"ada"});
        CHECK(mini.draft("ada").empty());
        mini.type("ada", "for mini's ada");
        mini.take({});
    }
    KeptFile file = fileOf(path);
    CHECK(file.size() == 2 && kept(file, "studio", "ada") == "for studio's ada");
    CHECK(kept(file, "me@mini", "ada") == "for mini's ada");
    Window studio(path, "studio");
    studio.take({"ada"});
    CHECK(studio.draft("ada") == "for studio's ada");
    file = fileOf(path);
    CHECK(file.size() == 1 && kept(file, "me@mini", "ada") == "for mini's ada");
    privateFiles(dir);
}

// Processes changing the file at once: each keeps its own drafts, one change at a time, through the lock.
void racing(const std::string& dir) {
    const std::string path = dir + "/kept-drafts.txt";
    constexpr int kProcesses = 4;
    constexpr int kChanges = 40;
    std::vector<pid_t> children;
    for (int p = 0; p < kProcesses; ++p) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            const KeptDraftsStore store = fileKeptDraftsStore(path);
            int bad = 0;
            for (int i = 0; i < kChanges; ++i) {
                std::string error;
                const std::string name = "p" + std::to_string(p) + "n" + std::to_string(i);
                if (!saveKeptChange(store, KeptChange{"studio", {{name, "draft " + name}}, {}}, &error)) {
                    std::fprintf(stderr, "racing: %s\n", error.c_str());
                    ++bad;
                }
            }
            std::_Exit(bad == 0 ? 0 : 1);
        }
        CHECK(pid > 0);
        children.push_back(pid);
    }
    for (pid_t pid : children) {
        int status = 0;
        CHECK(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    const KeptFile file = fileOf(path);
    CHECK(file.size() == static_cast<std::size_t>(kProcesses * kChanges));
    for (int p = 0; p < kProcesses; ++p) {
        for (int i = 0; i < kChanges; ++i) {
            const std::string name = "p" + std::to_string(p) + "n" + std::to_string(i);
            const auto at = file.find({"studio", name});
            CHECK(at != file.end() && at->second == "draft " + name);
        }
    }
    privateFiles(dir);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: nexus_kept_drafts_functional <dir> <case>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string name = argv[2];
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    if (name == "two_windows") {
        twoWindows(dir);
    } else if (name == "two_servers") {
        twoServers(dir);
    } else if (name == "racing") {
        racing(dir);
    } else {
        std::fprintf(stderr, "nexus_kept_drafts_functional: no case %s\n", name.c_str());
        return 2;
    }
    if (failures == 0) {
        std::filesystem::remove_all(dir);
    }
    return failures == 0 ? 0 : 1;
}
