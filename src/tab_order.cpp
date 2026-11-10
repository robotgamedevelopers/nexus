#include "tab_order.h"

#include <cerrno>
#include <cstring>

#include "pr/file.h"

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

// The store in the file `name` in nexus's own directory under the user's application-support directory.
TabOrderStore fileStoreNamed(std::string_view name) {
    PrRawString data = prFileUserDirectory(PrFileUserDirectory_Data);
    std::string path;
    if (!prRawStringIsNull(&data)) {
        PrStringView v = prRawStringView(&data);
        path = std::string(v.data, v.size) + "/nexus/" + std::string(name);
    }
    prRawStringReset(&data);
    if (path.empty()) {
        TabOrderStore none;
        none.read = [](std::string*, std::string* error) {
            *error = "the platform names no application-support directory";
            return false;
        };
        none.write = [](const std::string&, std::string* error) {
            *error = "the platform names no application-support directory";
            return false;
        };
        return none;
    }
    return fileTabOrderStore(path);
}

// The first line of the selected tab's file: what it is, and its format's version.
constexpr std::string_view kSelectedHeader = "nexus selected tab 1";
constexpr std::string_view kFriendPrefix = "friend ";

} // namespace

TabOrderStore fileTabOrderStore() {
    return fileStoreNamed("tab-order.txt");
}

TabOrderStore fileSelectedTabStore() {
    return fileStoreNamed("selected-tab.txt");
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
    if (tab == nullptr || tab->kind == TabKind::Sprint)
        text += "sprint";
    else if (tab->kind == TabKind::Terminal)
        text += "terminal";
    else
        text += std::string(kFriendPrefix) + tab->name;
    return text + '\n';
}

bool parseSelectedTab(std::string_view text, SavedTab* saved, std::string* error) {
    *saved = SavedTab{};
    if (text.find('\0') != std::string_view::npos) {
        *error = "it holds a NUL byte";
        return false;
    }
    std::size_t end = text.find('\n');
    if (text.substr(0, end) != kSelectedHeader) {
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
    if (line == "terminal") {
        saved->kind = TabKind::Terminal;
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
    *error = "its second line \"" + std::string(line) + "\" is not sprint, terminal or friend <name>";
    return false;
}

std::size_t savedTabPosition(const Model& model, const SavedTab& saved) {
    if (saved.kind == TabKind::Friend) {
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            if (model.tab(i).kind == TabKind::Friend && model.tab(i).name == saved.name)
                return i;
        }
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

} // namespace nexus
