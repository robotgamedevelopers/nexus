#include "tab_order.h"

#include <cerrno>
#include <cstring>

#include "pr/file.h"

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

std::vector<FriendInfo> loadTabOrder(const TabOrderStore& store, const std::vector<FriendInfo>& loaded,
        std::string* warning) {
    warning->clear();
    std::string text;
    std::string error;
    std::vector<std::string> names;
    if (!store.read(&text, &error)) {
        if (!error.empty())
            *warning = "the saved tab order " + store.where + " is not used: " + error +
                "; deleting it resets the order";
        return loaded;
    }
    if (!parseTabOrder(text, &names, &error)) {
        *warning = "the saved tab order " + store.where + " is not used: " + error + "; deleting it resets the order";
        return loaded;
    }
    return applyTabOrder(loaded, names);
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

std::size_t loadSelectedTab(const TabOrderStore& store, const Model& model, std::string* warning) {
    warning->clear();
    std::string text;
    std::string error;
    SavedTab saved;
    if (!store.read(&text, &error)) {
        if (!error.empty())
            *warning = "the saved selected tab " + store.where + " is not used: " + error +
                "; deleting it resets the selection to the sprint tab";
        return 0;
    }
    if (!parseSelectedTab(text, &saved, &error)) {
        *warning = "the saved selected tab " + store.where + " is not used: " + error +
            "; deleting it resets the selection to the sprint tab";
        return 0;
    }
    return savedTabPosition(model, saved);
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

} // namespace nexus
