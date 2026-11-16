// Functional test of nexus-server's home (src/server/home.h) as the real program: with HOME set and no --dir, `new`
// makes ~/.nexus and ~/.nexus/server, each mode 0700, and keeps everything in ~/.nexus/server (and nothing else at the
// top of ~/.nexus); a malformed or unreadable ~/.nexus/server/config stops `serve`, naming the file and the line; and
// with no HOME the server refuses, --dir or not. Real processes, files and a socket, so functional.
//
// Usage: server_home <nexus-server> <home>, with <home> a directory of this test's own (a Unix socket's path must be
// short, so it is under /tmp, named for the build tree: tests/link/CMakeLists.txt).

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Runs `command` through /bin/sh; its standard output and error in `out`, and its exit status.
int run(const std::string& command, std::string* out) {
    out->clear();
    FILE* p = popen((command + " 2>&1").c_str(), "r");
    if (p == nullptr) {
        return -1;
    }
    char buf[512];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
        out->append(buf, n);
    }
    const int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int modeOf(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 ? int(st.st_mode & 0777) : -1;
}

bool exists(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0;
}

std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return out;
    }
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") {
            out.push_back(name);
        }
    }
    closedir(d);
    return out;
}

bool writeFile(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: server_home <nexus-server> <home>\n");
        return 2;
    }
    const std::string server = argv[1];
    const std::string home = argv[2];
    const std::string top = home + "/.nexus";
    const std::string dir = top + "/server";
    std::string out;
    // What an earlier run left: its server stopped, its home removed.
    CHECK(setenv("HOME", home.c_str(), 1) == 0);
    run("'" + server + "' stop", &out);
    run("rm -rf '" + home + "'", &out);
    CHECK(mkdir(home.c_str(), 0755) == 0);

    // `new` with no --dir: the server's home is made, 0700 at both levels, and holds what the server keeps.
    CHECK(run("'" + server + "' new", &out) == 0 && has(out, "NEXUS CONNECT"));
    CHECK(modeOf(top) == 0700 && modeOf(dir) == 0700);
    CHECK(exists(dir + "/server.sock") && modeOf(dir + "/server.sock") == 0600);
    CHECK(exists(dir + "/store") && exists(dir + "/server.log"));
    CHECK(entries(top) == std::vector<std::string>{"server"});
    CHECK(run("'" + server + "' stop", &out) == 0);
    for (int i = 0; i < 100 && exists(dir + "/server.sock"); ++i) {
        usleep(50 * 1000);
    }

    // A config the server cannot use stops it before it serves, naming the file and the line.
    CHECK(writeFile(dir + "/config", "# nothing yet\nport\n"));
    CHECK(run("'" + server + "' serve", &out) == 1);
    CHECK(has(out, dir + "/config line 2:") && has(out, "no value"));
    // An unknown key is only logged.
    CHECK(writeFile(dir + "/config", "port 9\n"));
    CHECK(run("'" + server + "' new", &out) == 0 && has(out, "NEXUS CONNECT"));
    CHECK(run("'" + server + "' stop", &out) == 0);
    for (int i = 0; i < 100 && exists(dir + "/server.sock"); ++i) {
        usleep(50 * 1000);
    }
    CHECK(run("cat '" + dir + "/server.log'", &out) == 0 && has(out, dir + "/config line 1:") &&
        has(out, "\"port\" is not a setting"));
    // An unreadable config is refused (not as root, who reads everything).
    if (geteuid() != 0) {
        CHECK(chmod((dir + "/config").c_str(), 0000) == 0);
        CHECK(run("'" + server + "' serve", &out) == 1 && has(out, "cannot read " + dir + "/config"));
        CHECK(chmod((dir + "/config").c_str(), 0600) == 0);
    }

    // Old ~/.nexus layout migrates once into ~/.nexus/server on `new`, keeping ~/.nexus/client untouched.
    run("rm -rf '" + home + "'", &out);
    CHECK(mkdir(home.c_str(), 0755) == 0 && mkdir(top.c_str(), 0700) == 0);
    CHECK(mkdir((top + "/client").c_str(), 0700) == 0);
    CHECK(writeFile(top + "/client/config", "server local\n"));
    CHECK(writeFile(top + "/config", "port 7\n"));
    CHECK(run("'" + server + "' new", &out) == 0 && has(out, "NEXUS CONNECT"));
    CHECK(exists(dir + "/config") && !exists(top + "/config"));
    CHECK(exists(top + "/client/config"));
    CHECK(run("'" + server + "' store check", &out) == 0 && has(out, "OK store"));
    CHECK(run("'" + server + "' stop", &out) == 0);
    for (int i = 0; i < 100 && exists(dir + "/server.sock"); ++i) {
        usleep(50 * 1000);
    }

    // No HOME: refused, with --dir too.
    CHECK(unsetenv("HOME") == 0);
    CHECK(run("'" + server + "' stop", &out) == 1 && has(out, "HOME is not set"));
    CHECK(run("'" + server + "' stop --dir '" + dir + "'", &out) == 1 && has(out, "HOME is not set"));
    CHECK(setenv("HOME", home.c_str(), 1) == 0);

    run("rm -rf '" + home + "'", &out);
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("server_home: all checks passed\n");
    return 0;
}
