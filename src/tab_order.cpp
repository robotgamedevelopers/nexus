#include "tab_order.h"

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "pr/file.h"

#include "accounts/registry.h"
#include "client_home.h"

namespace nexus {

namespace {

// The first line of the file: what it is, and its format's version.
constexpr std::string_view kHeader = "nexus tab order 1";

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

} // namespace

std::vector<FriendInfo> applyTabOrder(const std::vector<FriendInfo>& loaded, const std::vector<std::string>& saved) {
    std::vector<FriendInfo> out;
    std::vector<bool> placed(loaded.size(), false);
    for (const std::string& name : saved) {
        for (std::size_t i = 0; i < loaded.size(); ++i) {
            if (!placed[i] && loaded[i].name == name) {
                placed[i] = true;
                out.push_back(loaded[i]);
                break;
            }
        }
    }
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        if (!placed[i])
            out.push_back(loaded[i]);
    }
    return out;
}

std::vector<std::string> friendTabNames(const Model& model) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind == TabKind::Friend)  // a terminal tab is never saved
            names.push_back(model.tab(i).name);
    }
    return names;
}

std::string formatTabOrder(const std::vector<std::string>& names) {
    std::string text(kHeader);
    text += '\n';
    for (const std::string& name : names)
        text += name + '\n';
    return text;
}

bool parseTabOrder(std::string_view text, std::vector<std::string>* names, std::string* error) {
    names->clear();
    if (text.find('\0') != std::string_view::npos) {
        *error = "it holds a NUL byte";
        return false;
    }
    std::size_t end = text.find('\n');
    std::string_view first = text.substr(0, end);
    if (first != kHeader) {
        *error = "its first line is not \"" + std::string(kHeader) + "\"";
        return false;
    }
    while (end != std::string_view::npos && end + 1 < text.size()) {
        std::size_t start = end + 1;
        end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? end : end - start);
        if (!line.empty())
            names->emplace_back(line);
    }
    return true;
}

TabOrderStore fileTabOrderStore(std::string path) {
    TabOrderStore store;
    store.where = path;
    store.read = [path](std::string* text, std::string* error) {
        error->clear();
        PrFileInfo info{};
        if (!prFileInfo(pr(path), &info))
            return false;           // nothing saved yet
        PrByteView bytes{};
        if (!prFileMap(pr(path), &bytes)) {
            *error = "it cannot be read";
            return false;
        }
        text->assign(reinterpret_cast<const char*>(bytes.data), bytes.size);
        prFileUnmap(&bytes);
        return true;
    };
    store.write = [path](const std::string& text, std::string* error) {
        std::string_view parent(prFilePathParent(pr(path)).data, prFilePathParent(pr(path)).size);
        errno = 0;
        if (!prFileCreateDirectory(pr(parent)) ||
                !prFileWrite(pr(path), PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()})) {
            // Prax's file calls return only a bool; errno is the platform's last word on why (a request for Prax: an
            // error code from prFileWrite and prFileCreateDirectory).
            *error = errno != 0 ? std::strerror(errno) : "the file could not be written";
            return false;
        }
        return true;
    };
    return store;
}

namespace {

// The store in the file `name` in the window's home, ~/.nexus/client (client_home.h), which a write makes (mode 0700)
// if it is missing.
TabOrderStore fileStoreNamed(std::string_view name) {
    std::string dir;
    std::string why;
    if (!clientHome(&dir, &why)) {
        TabOrderStore none;
        none.read = [why](std::string*, std::string* error) {
            *error = why;
            return false;
        };
        none.write = [why](const std::string&, std::string* error) {
            *error = why;
            return false;
        };
        return none;
    }
#if defined(_WIN32)
    TabOrderStore store = fileTabOrderStore(dir + "\\" + std::string(name));
#else
    TabOrderStore store = fileTabOrderStore(dir + "/" + std::string(name));
#endif
    store.write = [dir, write = store.write](const std::string& text, std::string* error) {
        return makeClientHome(dir, error) && write(text, error);
    };
    return store;
}

// The first line of the selected tab's file: what it is, and its format's version. Version 1 saved a terminal as
// "terminal", by no place, since terminals were not kept.
constexpr std::string_view kSelectedHeader = "nexus selected tab 2";
constexpr std::string_view kSelectedHeader1 = "nexus selected tab 1";
constexpr std::string_view kFriendPrefix = "friend ";
constexpr std::string_view kTerminalPrefix = "terminal ";

// The first line of the terminals' file: what it is, and its format's version.
constexpr std::string_view kTerminalsHeader = "nexus terminals 1";
constexpr std::string_view kTerminalWord = "terminal";

// The first line of the text scale's file: what it is, and its format's version.
constexpr std::string_view kTextScaleHeader = "nexus text scale 1";

// The most digits a saved number of steps, or a terminal's place, may have: far past any range, and short of
// overflowing an int.
constexpr std::size_t kMaxStepDigits = 6;

} // namespace

TabOrderStore fileTabOrderStore() {
    return fileStoreNamed("tab-order.txt");
}

TabOrderStore fileSelectedTabStore() {
    return fileStoreNamed("selected-tab.txt");
}

TabOrderStore fileTextScaleStore() {
    return fileStoreNamed("text-scale.txt");
}

TabOrderStore fileTerminalsStore() {
    return fileStoreNamed("terminals.txt");
}

std::vector<std::string> loadTabOrderNames(const TabOrderStore& store, std::string* warning) {
    warning->clear();
    std::string text;
    std::string error;
    std::vector<std::string> names;
    if (!store.read(&text, &error)) {
        if (!error.empty()) {
            *warning = "the saved tab order " + store.where + " is not used: " + error +
                "; deleting it resets the order";
        }
        return {};
    }
    if (!parseTabOrder(text, &names, &error)) {
        *warning = "the saved tab order " + store.where + " is not used: " + error + "; deleting it resets the order";
        return {};
    }
    return names;
}

std::vector<FriendInfo> loadTabOrder(const TabOrderStore& store, const std::vector<FriendInfo>& loaded,
        std::string* warning) {
    return applyTabOrder(loaded, loadTabOrderNames(store, warning));
}

bool saveTabOrder(const TabOrderStore& store, const Model& model, std::string* error) {
    std::string why;
    if (store.write(formatTabOrder(friendTabNames(model)), &why))
        return true;
    *error = "the tab order could not be saved to " + store.where + ": " + why;
    return false;
}

std::string formatSelectedTab(const Model& model) {
    std::string text(kSelectedHeader);
    text += '\n';
    const Tab* tab = model.tab_count() != 0 ? &model.tab(model.active()) : nullptr;
    if (tab == nullptr || tab->kind == TabKind::Sprint) {
        text += "sprint";
    } else if (tab->kind == TabKind::Terminal) {
        std::size_t place = 0;
        for (std::size_t i = 0; i <= model.active(); ++i)
            place += model.tab(i).kind == TabKind::Terminal ? 1 : 0;
        text += std::string(kTerminalPrefix) + std::to_string(place);
    } else {
        text += std::string(kFriendPrefix) + tab->name;
    }
    return text + '\n';
}

bool parseSelectedTab(std::string_view text, SavedTab* saved, std::string* error) {
    *saved = SavedTab{};
    if (text.find('\0') != std::string_view::npos) {
        *error = "it holds a NUL byte";
        return false;
    }
    std::size_t end = text.find('\n');
    const std::string_view header = text.substr(0, end);
    if (header != kSelectedHeader && header != kSelectedHeader1) {
        *error = "its first line is not \"" + std::string(kSelectedHeader) + "\"";
        return false;
    }
    std::string_view line = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    line = line.substr(0, line.find('\n'));
    if (line.empty()) {
        *error = "it names no tab";
        return false;
    }
    if (line == "sprint")
        return true;
    if (header == kSelectedHeader1 && line == kTerminalWord) {
        saved->kind = TabKind::Terminal;
        return true;
    }
    if (header == kSelectedHeader && line.substr(0, kTerminalPrefix.size()) == kTerminalPrefix) {
        // A place from 1, in at most kMaxStepDigits digits: far past any number of tabs.
        const std::string_view digits = line.substr(kTerminalPrefix.size());
        bool ok = !digits.empty() && digits.size() <= kMaxStepDigits && digits[0] != '0';
        std::size_t place = 0;
        for (std::size_t k = 0; k < digits.size() && ok; ++k) {
            ok = digits[k] >= '0' && digits[k] <= '9';
            place = place * 10 + std::size_t(digits[k] - '0');
        }
        if (!ok) {
            *error = "its terminal line \"" + std::string(line) + "\" does not give a place from 1";
            return false;
        }
        saved->kind = TabKind::Terminal;
        saved->terminal = place;
        return true;
    }
    if (line.substr(0, kFriendPrefix.size()) == kFriendPrefix) {
        if (line.size() == kFriendPrefix.size()) {
            *error = "its friend line has no name";
            return false;
        }
        saved->kind = TabKind::Friend;
        saved->name = std::string(line.substr(kFriendPrefix.size()));
        return true;
    }
    *error = "its second line \"" + std::string(line) + "\" is not sprint, terminal <n> or friend <name>";
    return false;
}

std::size_t savedTabPosition(const Model& model, const SavedTab& saved) {
    std::size_t terminals = 0;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        const Tab& tab = model.tab(i);
        if (saved.kind == TabKind::Friend && tab.kind == TabKind::Friend && tab.name == saved.name)
            return i;
        if (saved.kind == TabKind::Terminal && tab.kind == TabKind::Terminal && ++terminals == saved.terminal)
            return i;
    }
    return 0;
}

SavedTab loadSavedTab(const TabOrderStore& store, std::string* warning) {
    warning->clear();
    std::string text;
    std::string error;
    SavedTab saved;
    if (!store.read(&text, &error)) {
        if (!error.empty()) {
            *warning = "the saved selected tab " + store.where + " is not used: " + error +
                "; deleting it resets the selection to the first tab";
        }
        return SavedTab{};
    }
    if (!parseSelectedTab(text, &saved, &error)) {
        *warning = "the saved selected tab " + store.where + " is not used: " + error +
            "; deleting it resets the selection to the first tab";
        return SavedTab{};
    }
    return saved;
}

std::size_t loadSelectedTab(const TabOrderStore& store, const Model& model, std::string* warning) {
    return savedTabPosition(model, loadSavedTab(store, warning));
}

bool saveSelectedTab(const TabOrderStore& store, const Model& model, std::string* error) {
    std::string why;
    if (store.write(formatSelectedTab(model), &why))
        return true;
    *error = "the selected tab could not be saved to " + store.where + ": " + why;
    return false;
}

std::string formatTextScale(int steps) {
    return std::string(kTextScaleHeader) + '\n' + std::to_string(steps) + '\n';
}

bool parseTextScale(std::string_view text, int* steps, std::string* error) {
    *steps = 0;
    if (text.find('\0') != std::string_view::npos) {
        *error = "it holds a NUL byte";
        return false;
    }
    std::size_t end = text.find('\n');
    if (text.substr(0, end) != kTextScaleHeader) {
        *error = "its first line is not \"" + std::string(kTextScaleHeader) + "\"";
        return false;
    }
    std::string_view line = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    line = line.substr(0, line.find('\n'));
    if (line.empty()) {
        *error = "it names no scale";
        return false;
    }
    const std::size_t sign = line[0] == '-' ? 1 : 0;
    bool digits = line.size() > sign && line.size() - sign <= kMaxStepDigits;
    for (std::size_t k = sign; k < line.size() && digits; ++k)
        digits = line[k] >= '0' && line[k] <= '9';
    if (!digits) {
        *error = "its second line \"" + std::string(line) + "\" is not a whole number of steps";
        return false;
    }
    int value = 0;
    for (std::size_t k = sign; k < line.size(); ++k)
        value = value * 10 + (line[k] - '0');
    *steps = sign != 0 ? -value : value;
    return true;
}

int loadTextScale(const TabOrderStore& store, int lo, int hi, std::string* warning) {
    warning->clear();
    std::string text;
    std::string error;
    int steps = 0;
    if (!store.read(&text, &error)) {
        if (!error.empty())
            *warning = "the saved text scale " + store.where + " is not used: " + error +
                "; deleting it resets the scale";
        return 0;
    }
    if (!parseTextScale(text, &steps, &error)) {
        *warning = "the saved text scale " + store.where + " is not used: " + error + "; deleting it resets the scale";
        return 0;
    }
    if (steps < lo || steps > hi) {
        const int clamped = steps < lo ? lo : hi;
        *warning = "the saved text scale " + store.where + " is " + std::to_string(steps) + " steps, outside " +
            std::to_string(lo) + " to " + std::to_string(hi) + "; using " + std::to_string(clamped);
        return clamped;
    }
    return steps;
}

bool saveTextScale(const TabOrderStore& store, int steps, std::string* error) {
    std::string why;
    if (store.write(formatTextScale(steps), &why))
        return true;
    *error = "the text scale could not be saved to " + store.where + ": " + why;
    return false;
}

std::vector<std::string> terminalDirectories(const Model& model,
        const std::function<std::string(std::size_t id)>& directoryOf) {
    std::vector<std::string> directories;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind != TabKind::Terminal)
            continue;
        std::string dir = directoryOf(model.tab(i).id);
        if (dir.find('\n') != std::string::npos || dir.find('\0') != std::string::npos)
            dir.clear();
        directories.push_back(std::move(dir));
    }
    return directories;
}

std::string formatTerminals(const std::vector<std::string>& directories) {
    std::string text(kTerminalsHeader);
    text += '\n';
    for (const std::string& dir : directories)
        text += dir.empty() ? std::string(kTerminalWord) + '\n' : std::string(kTerminalPrefix) + dir + '\n';
    return text;
}

bool parseTerminals(std::string_view text, std::vector<std::string>* directories, std::string* error) {
    directories->clear();
    if (text.find('\0') != std::string_view::npos) {
        *error = "it holds a NUL byte";
        return false;
    }
    std::size_t end = text.find('\n');
    if (text.substr(0, end) != kTerminalsHeader) {
        *error = "its first line is not \"" + std::string(kTerminalsHeader) + "\"";
        return false;
    }
    while (end != std::string_view::npos && end + 1 < text.size()) {
        std::size_t start = end + 1;
        end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? end : end - start);
        if (line.empty())
            continue;
        if (line == kTerminalWord) {
            directories->emplace_back();
        } else if (line.size() > kTerminalPrefix.size() && line.substr(0, kTerminalPrefix.size()) == kTerminalPrefix) {
            directories->emplace_back(line.substr(kTerminalPrefix.size()));
        } else {
            *error = "its line \"" + std::string(line) + "\" is not terminal or terminal <directory>";
            directories->clear();
            return false;
        }
    }
    return true;
}

std::vector<std::string> loadTerminals(const TabOrderStore& store, std::string* warning) {
    warning->clear();
    std::string text;
    std::string error;
    std::vector<std::string> directories;
    if (!store.read(&text, &error)) {
        if (!error.empty())
            *warning = "the saved terminals " + store.where + " are not used: " + error +
                "; deleting it reopens none";
        return {};
    }
    if (!parseTerminals(text, &directories, &error)) {
        *warning = "the saved terminals " + store.where + " are not used: " + error + "; deleting it reopens none";
        return {};
    }
    if (directories.size() > kMaxSavedTerminals) {
        *warning = "the saved terminals " + store.where + " name " + std::to_string(directories.size()) +
            " terminals; reopening the first " + std::to_string(kMaxSavedTerminals);
        directories.resize(kMaxSavedTerminals);
    }
    return directories;
}

bool saveTerminals(const TabOrderStore& store, const std::vector<std::string>& directories, std::string* error) {
    std::string why;
    if (store.write(formatTerminals(directories), &why))
        return true;
    *error = "the terminals could not be saved to " + store.where + ": " + why;
    return false;
}

namespace {

// The first line of the kept drafts' file: what it is, and its format's version; and each draft's line's word.
constexpr std::string_view kKeptHeader = "nexus kept drafts 2";
constexpr std::string_view kDraftPrefix = "draft ";

// The most digits a draft's length may have: past any prompt (kPromptBytes), short of overflowing.
constexpr std::size_t kMaxLengthDigits = 12;

// The longest server a kept draft is keyed by (keptServer).
constexpr std::size_t kMaxKeptServer = 255;

} // namespace

void applyKeptChange(KeptFile* file, const KeptChange& change) {
    for (const auto& [name, draft] : change.taken) {
        const auto at = file->find({change.server, name});
        if (at != file->end() && at->second == draft) {
            file->erase(at);
        }
    }
    for (const auto& [name, draft] : change.put) {
        if (draft.empty()) {
            continue;
        }
        std::string& kept = (*file)[{change.server, name}];
        if (kept.empty()) {
            kept = draft;
        } else if (kept != draft) {
            kept += kKeptJoin;
            kept += draft;
        }
    }
}

bool keptServer(std::string_view server) {
    if (server.empty() || server.size() > kMaxKeptServer) {
        return false;
    }
    for (const char c : server) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= ' ' || u == 0x7f) {
            return false;
        }
    }
    return true;
}

std::map<std::string, std::string> keptFor(const KeptFile& file, const std::string& server) {
    std::map<std::string, std::string> drafts;
    for (auto at = file.lower_bound({server, std::string()}); at != file.end() && at->first.first == server; ++at) {
        drafts.emplace(at->first.second, at->second);
    }
    return drafts;
}

std::string formatKeptDrafts(const KeptFile& file) {
    std::string text(kKeptHeader);
    text += '\n';
    for (const auto& [key, draft] : file) {
        text += std::string(kDraftPrefix) + key.first + ' ' + key.second + ' ' + std::to_string(draft.size()) + '\n';
        text += draft;
        text += '\n';
    }
    return text;
}

bool parseKeptDrafts(std::string_view text, KeptFile* file, std::string* error) {
    file->clear();
    auto refuse = [&](const std::string& why) {
        *error = why;
        file->clear();
        return false;
    };
    std::size_t end = text.find('\n');
    if (end == std::string_view::npos || text.substr(0, end) != kKeptHeader) {
        return refuse("its first line is not \"" + std::string(kKeptHeader) + "\"");
    }
    std::size_t at = end + 1;
    while (at < text.size()) {
        end = text.find('\n', at);
        if (end == std::string_view::npos) {
            return refuse("its last line has no line feed");
        }
        const std::string_view line = text.substr(at, end - at);
        const std::size_t first = line.find(' ', kDraftPrefix.size());
        const std::size_t last = line.rfind(' ');
        if (line.substr(0, kDraftPrefix.size()) != kDraftPrefix || first == std::string_view::npos ||
                last == first) {
            return refuse("a line is not draft <server> <friend> <bytes>");
        }
        const std::string_view server = line.substr(kDraftPrefix.size(), first - kDraftPrefix.size());
        const std::string_view name = line.substr(first + 1, last - first - 1);
        const std::string_view digits = line.substr(last + 1);
        if (!keptServer(server) || !accounts::isName(name)) {
            return refuse("a line is not draft <server> <friend> <bytes>");
        }
        if (digits.empty() || digits.size() > kMaxLengthDigits ||
                digits.find_first_not_of("0123456789") != std::string_view::npos) {
            return refuse("a draft's length is not a number");
        }
        const std::size_t length = static_cast<std::size_t>(std::stoull(std::string(digits)));
        const std::size_t from = end + 1;
        if (length >= text.size() - from || text[from + length] != '\n') {
            return refuse("a draft is cut short");
        }
        if (!file->emplace(std::make_pair(std::string(server), std::string(name)),
                           std::string(text.substr(from, length))).second) {
            return refuse("it keeps two drafts for one friend of one server");
        }
        at = from + length + 1;
    }
    return true;
}

#if defined(_WIN32)

KeptDraftsStore fileKeptDraftsStore(std::string path) {
    KeptDraftsStore store;
    store.where = path;
    store.read = [](KeptFile*, std::string* error) {
        *error = "drafts are not kept on Windows, where the window has no server link";
        return false;
    };
    store.update = [](const KeptChange&, std::string* error) {
        *error = "drafts are not kept on Windows, where the window has no server link";
        return false;
    };
    return store;
}

#else

namespace {

// The whole of the file at `path`: true with `text` (empty, and `exists` false, when there is none), false with
// `error`.
bool readWhole(const std::string& path, std::string* text, bool* exists, std::string* error) {
    text->clear();
    *exists = false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return true;
        }
        *error = std::strerror(errno);
        return false;
    }
    *exists = true;
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = std::strerror(errno);
            ::close(fd);
            return false;
        }
        text->append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return true;
}

// `text` into `path` whole: a file beside it, 0600, written and synced, then renamed over it.
bool writeWhole(const std::string& path, const std::string& text, std::string* error) {
    const std::string temp = path + "." + std::to_string(static_cast<long>(::getpid())) + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        *error = std::strerror(errno);
        return false;
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = std::strerror(errno);
            ::close(fd);
            ::unlink(temp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fchmod(fd, 0600) != 0 || ::fsync(fd) != 0) {
        *error = std::strerror(errno);
        ::close(fd);
        ::unlink(temp.c_str());
        return false;
    }
    ::close(fd);
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        *error = std::strerror(errno);
        ::unlink(temp.c_str());
        return false;
    }
    return true;
}

// The lock beside `path`: kept-drafts.lock for kept-drafts.txt.
std::string lockPathOf(const std::string& path) {
    const std::string_view txt = ".txt";
    if (path.size() > txt.size() && path.compare(path.size() - txt.size(), txt.size(), txt) == 0) {
        return path.substr(0, path.size() - txt.size()) + ".lock";
    }
    return path + ".lock";
}

} // namespace

KeptDraftsStore fileKeptDraftsStore(std::string path) {
    KeptDraftsStore store;
    store.where = path;
    store.read = [path](KeptFile* file, std::string* error) {
        file->clear();
        std::string text;
        bool exists = false;
        if (!readWhole(path, &text, &exists, error)) {
            return false;
        }
        return !exists || parseKeptDrafts(text, file, error);
    };
    store.update = [path, lock = lockPathOf(path)](const KeptChange& change, std::string* error) {
        const int fd = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            *error = "its lock " + lock + " cannot be opened (" + std::strerror(errno) + ")";
            return false;
        }
        int locked = 0;
        while ((locked = ::flock(fd, LOCK_EX)) != 0 && errno == EINTR) {
        }
        if (locked != 0) {
            *error = "its lock " + lock + " cannot be taken (" + std::strerror(errno) + ")";
            ::close(fd);
            return false;
        }
        if (::fchmod(fd, 0600) != 0) {
            *error = "its lock " + lock + " cannot be made private (" + std::strerror(errno) + ")";
            ::close(fd);
            return false;
        }
        KeptFile file;
        std::string text;
        bool exists = false;
        bool ok = readWhole(path, &text, &exists, error) && (!exists || parseKeptDrafts(text, &file, error));
        if (ok) {
            applyKeptChange(&file, change);
            ok = writeWhole(path, formatKeptDrafts(file), error);
        }
        ::close(fd);    // and with it the lock
        return ok;
    };
    return store;
}

#endif

KeptDraftsStore fileKeptDraftsStore() {
    std::string dir;
    std::string why;
    if (!clientHome(&dir, &why)) {
        KeptDraftsStore none;
        none.read = [why](KeptFile*, std::string* error) {
            *error = why;
            return false;
        };
        none.update = [why](const KeptChange&, std::string* error) {
            *error = why;
            return false;
        };
        return none;
    }
#if defined(_WIN32)
    KeptDraftsStore store = fileKeptDraftsStore(dir + "\\kept-drafts.txt");
#else
    KeptDraftsStore store = fileKeptDraftsStore(dir + "/kept-drafts.txt");
#endif
    store.update = [dir, update = store.update](const KeptChange& change, std::string* error) {
        return makeClientHome(dir, error) && update(change, error);
    };
    return store;
}

std::map<std::string, std::string> loadKeptDrafts(const KeptDraftsStore& store, const std::string& server,
        bool* readable, std::string* warning) {
    warning->clear();
    *readable = true;
    KeptFile file;
    std::string error;
    if (!store.read(&file, &error)) {
        *readable = false;
        *warning = "the kept drafts " + store.where + " are not used: " + error +
            "; the file is left as it is, and this run changes no draft in it";
        return {};
    }
    return keptFor(file, server);
}

bool saveKeptChange(const KeptDraftsStore& store, const KeptChange& change, std::string* error) {
    std::string why;
    if (store.update(change, &why)) {
        return true;
    }
    *error = "the kept drafts could not be saved to " + store.where + ": " + why;
    return false;
}

// One store's waiting write: the newest text, while `waiting`.
struct StoreSlot {
    std::string where;
    std::function<bool(const std::string& text, std::string* error)> write;
    std::string text;
    bool waiting = false;
};

// A change of the kept drafts handed to the writer: made once, in its turn.
struct KeptJob {
    std::string where;
    std::function<bool(std::string* error)> make;
};

struct StoreWriter::Impl {
    std::mutex mutex;
    std::condition_variable wake;       // a write waits, or the writer ends
    std::condition_variable done;       // every write handed over is made
    std::deque<std::shared_ptr<StoreSlot>> queue;   // the stores with a write waiting, oldest first
    std::deque<KeptJob> changes;        // the kept drafts' changes, oldest first, each made
    std::size_t writing = 0;            // writes taken from the queues and not yet made
    bool ending = false;
    std::thread thread;

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return ending || !queue.empty() || !changes.empty(); });
            if (queue.empty() && changes.empty()) {
                return;     // ending, with nothing left to write
            }
            if (!changes.empty()) {
                KeptJob job = std::move(changes.front());
                changes.pop_front();
                ++writing;
                lock.unlock();
                std::string error;
                if (!job.make(&error)) {
                    std::fprintf(stderr, "nexus: the kept drafts %s were not changed (%s); the drafts of this change "
                        "are kept in this window until it quits\n", job.where.c_str(), error.c_str());
                }
                lock.lock();
                --writing;
                if (queue.empty() && changes.empty() && writing == 0) {
                    done.notify_all();
                }
                continue;
            }
            std::shared_ptr<StoreSlot> slot = queue.front();
            queue.pop_front();
            std::string text = std::move(slot->text);
            slot->waiting = false;
            ++writing;
            lock.unlock();
            std::string error;
            if (!slot->write(text, &error)) {
                std::fprintf(stderr, "nexus: %s could not be written (%s); it is written again at the next change\n",
                    slot->where.empty() ? "a kept preference" : slot->where.c_str(), error.c_str());
            }
            lock.lock();
            --writing;
            if (queue.empty() && changes.empty() && writing == 0) {
                done.notify_all();
            }
        }
    }
};

StoreWriter::StoreWriter() : impl_(std::make_unique<Impl>()) {
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
}

StoreWriter::~StoreWriter() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->ending = true;
    }
    impl_->wake.notify_all();
    impl_->thread.join();
}

TabOrderStore StoreWriter::wrap(TabOrderStore store) {
    auto slot = std::make_shared<StoreSlot>();
    slot->where = store.where;
    slot->write = std::move(store.write);
    Impl* impl = impl_.get();
    store.write = [impl, slot](const std::string& text, std::string*) {
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            slot->text = text;
            if (!slot->waiting) {
                slot->waiting = true;
                impl->queue.push_back(slot);
            }
        }
        impl->wake.notify_one();
        return true;
    };
    return store;
}

KeptDraftsStore StoreWriter::wrap(KeptDraftsStore store) {
    Impl* impl = impl_.get();
    store.update = [impl, where = store.where, update = std::move(store.update)](const KeptChange& change,
                                                                               std::string*) {
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->changes.push_back(KeptJob{where, [update, change](std::string* error) {
                return update(change, error);
            }});
        }
        impl->wake.notify_one();
        return true;
    };
    return store;
}

void StoreWriter::flush() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->done.wait(lock, [this] { return impl_->queue.empty() && impl_->changes.empty() && impl_->writing == 0; });
}

} // namespace nexus
