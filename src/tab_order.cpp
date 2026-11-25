#include "tab_order.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "pr/file.h"
#include "pr/json.h"

#include "accounts/registry.h"
#include "client_home.h"
#include "sync/dashboards.h"

namespace nexus {

namespace {

// The first line of the file: what it is, and its format's version.
constexpr std::string_view kHeader = "nexus tab order 1";

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

} // namespace

std::vector<AIInfo> applyTabOrder(const std::vector<AIInfo>& loaded, const std::vector<std::string>& saved) {
    std::vector<AIInfo> out;
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

std::vector<std::string> aiTabNames(const Model& model) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).kind == TabKind::AI)  // a terminal tab is never saved
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
        PrBlob* blob = prFileOpen(pr(path));
        if (!blob) {
            *error = "it cannot be read";
            return false;
        }
        const auto bytes = prBlobBytes(blob);
        text->assign(reinterpret_cast<const char*>(bytes.data), bytes.size);
        prBlobRelease(blob);
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
// "terminal", by no place, since terminals were not kept. Versions 1 and 2 saved every dashboard as "sprint", from when
// there was one; version 3 saves a dashboard by its name, and "first" for a tab it keeps no name of.
constexpr std::string_view kSelectedHeader = "nexus selected tab 3";
constexpr std::string_view kSelectedHeader2 = "nexus selected tab 2";
constexpr std::string_view kSelectedHeader1 = "nexus selected tab 1";
constexpr std::string_view kAIPrefix = "ai ";
constexpr std::string_view kDashboardPrefix = "dashboard ";
constexpr std::string_view kFirstWord = "first";
// The one dashboard's word in a selected tab file of version 1 or 2, and its key in a layout saved before dashboards
// were kept by name: read as the first dashboard's (TabLayout::restore), and saved by name at the next save.
constexpr std::string_view kOldSprint = "sprint";
constexpr std::string_view kDashboardKey = "dashboard:";
// What the saved files said before the friend names became AI: the selected tab's "friend <name>", the layout's
// "friend:<name>" keys and "friends" group. Read as the AI's, and written the new way at the next save; the word
// stays here only.
constexpr std::string_view kOldAIPrefix = "friend ";
constexpr std::string_view kOldAIKey = "friend:";
constexpr std::string_view kOldAIGroup = "friends";

// A layout key or group as this version names it.
std::string currentKey(std::string key) {
    return key.starts_with(kOldAIKey) ? "ai:" + key.substr(kOldAIKey.size()) : key;
}
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

TabOrderStore fileLayoutStore() {
    return fileStoreNamed("tab-layout.json");
}

TabOrderStore fileRefusedLayoutStore() {
    return fileStoreNamed("tab-layout.refused.json");
}

std::string formatLayout(const SavedLayout& layout) {
    std::unique_ptr<PrJsonWriter, decltype(&prJsonWriterDestroy)> w(prJsonWriterCreate(), prJsonWriterDestroy);
    prJsonWriterBeginObject(w.get());
    prJsonWriterKey(w.get(), pr("version"));
    prJsonWriterNumber(w.get(), 1);
    prJsonWriterKey(w.get(), pr("selected"));
    prJsonWriterString(w.get(), pr(layout.selected));
    prJsonWriterKey(w.get(), pr("tabs"));
    prJsonWriterBeginArray(w.get());
    for (const auto& row : layout.tabs) {
        prJsonWriterBeginArray(w.get());
        for (const auto* field : {&row.key, &row.group, &row.detail})
            prJsonWriterString(w.get(), pr(*field));
        prJsonWriterEndArray(w.get());
    }
    prJsonWriterEndArray(w.get());
    prJsonWriterEndObject(w.get());
    const auto text = prJsonWriterText(w.get());
    return std::string(text.data, text.size) + "\n";
}

bool parseLayout(std::string_view text, SavedLayout* layout, std::string* why) {
    std::string unused;
    if (why == nullptr) why = &unused;
    const auto refuse = [why](std::string reason) {
        *why = std::move(reason);
        return false;
    };
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> doc(prJsonCreate(), prJsonDestroy);
    auto* d = doc.get();
    if (!prJsonParse(d, pr(text), nullptr))
        return refuse("it is not JSON");
    if (prJsonNumber(d, prJsonMember(d, 0, pr("version")), 0) != 1)
        return refuse("it is not a version 1 layout");
    auto stringAt = [&](PrJsonValueIndex index, std::string& out) {
        if (prJsonKind(d, index) != PrJsonKind_String) return false;
        const auto value = prJsonString(d, index);
        out.assign(value.data ? value.data : "", value.size);
        return out.find('\0') == out.npos;
    };
    SavedLayout result;
    const auto tabs = prJsonMember(d, 0, pr("tabs"));
    if (prJsonKind(d, tabs) != PrJsonKind_Array || !stringAt(prJsonMember(d, 0, pr("selected")), result.selected))
        return refuse("it has no tabs list or no selected tab");
    result.selected = currentKey(std::move(result.selected));
    std::set<std::string> seen;
    for (auto i = prJsonFirst(d, tabs); i != kPrJsonMissing; i = prJsonAfter(d, tabs, i)) {
        LayoutTab row;
        if (prJsonKind(d, i) != PrJsonKind_Array || prJsonCount(d, i) != 3 ||
            !stringAt(prJsonElement(d, i, 0), row.key) || !stringAt(prJsonElement(d, i, 1), row.group) ||
            !stringAt(prJsonElement(d, i, 2), row.detail))
            return refuse("a tab is not three strings");
        row.key = currentKey(std::move(row.key));
        if (row.group == kOldAIGroup) row.group = "ais";
        if (!seen.insert(row.key).second) return refuse("the tab \"" + row.key + "\" is in it twice");
        const std::string_view key(row.key);
        if (key.starts_with("terminal:") || key.starts_with("extension:")) {
            auto number = key.substr(key.find(':') + 1);
            if (number.empty() || number.size() > 20 || number.front() == '0' ||
                number.find_first_not_of("0123456789") != number.npos ||
                (key.starts_with("extension:") && row.detail.empty()))
                return refuse("the tab \"" + row.key + "\" is not a terminal or extension this build knows");
        } else if (!(key == kOldSprint || key == "primary" ||
                     (key.starts_with(kDashboardKey) &&
                      sync::dashboardNameProblem(key.substr(kDashboardKey.size())).empty()) ||
                     (key.starts_with("ai:") && accounts::isName(key.substr(3)))) ||
                   !row.detail.empty()) {
            return refuse("the tab \"" + row.key + "\" is not a tab this build knows");
        }
        result.tabs.push_back(std::move(row));
    }
    if (!result.selected.empty() && !seen.contains(result.selected))
        return refuse("its selected tab \"" + result.selected + "\" is not among its tabs");
    *layout = std::move(result);
    return true;
}

LayoutLoad loadLayout(const TabOrderStore& store, const TabOrderStore& aside) {
    LayoutLoad load;
    std::string text, error;
    if (!store.read(&text, &error)) {
        if (!error.empty()) {
            load.keep = false;
            load.warning = "the saved tab layout " + store.where + " could not be read (" + error + "): the window starts "
                "without it and saves no layout until it is readable again, or deleted";
        }
        return load;
    }
    std::string why;
    if (parseLayout(text, &load.layout, &why)) {
        load.have = true;
        return load;
    }
    load.layout = {};
    std::string copied;
    if (aside.write(text, &copied)) {
        load.warning = "the saved tab layout " + store.where + " is refused (" + why + "): it is kept as " + aside.where +
            ", and the window starts without it and saves its layout anew";
        return load;
    }
    load.keep = false;
    load.warning = "the saved tab layout " + store.where + " is refused (" + why + ") and could not be kept as " +
        aside.where + " (" + copied + "): the window starts without it and leaves it in place, saving no layout this "
        "run; deleting it resets the layout";
    return load;
}

TabLayout::TabLayout(SavedLayout saved) : saved_(std::move(saved)), pending_(saved_.selected) {}

std::string TabLayout::keyOf(const Tab& tab) {
    if (tab.kind == TabKind::Dashboard) return std::string(kDashboardKey) + tab.name;
    if (tab.primaryChat()) return "primary";
    if (tab.kind == TabKind::AI) return "ai:" + tab.name;
    auto& key = localKeys_[tab.id];
    if (key.empty()) {
        const std::string prefix = tab.kind == TabKind::Terminal ? "terminal:" : "extension:";
        do {
            key = prefix + std::to_string(++serial_);
        } while (std::any_of(saved_.tabs.begin(), saved_.tabs.end(), [&](const auto& row) { return row.key == key; }));
    }
    return key;
}

std::vector<std::pair<std::size_t, std::string>> TabLayout::reopenTerminals(Model& model) {
    std::vector<std::pair<std::size_t, std::string>> out;
    for (const auto& row : saved_.tabs) {
        if (!row.key.starts_with("terminal:") || out.size() >= kMaxSavedTerminals) continue;
        const auto id = model.add_terminal();
        localKeys_[id] = row.key;
        out.emplace_back(id, row.detail);
    }
    return out;
}

void TabLayout::restore(Model& model, const std::function<bool(const std::string&, std::string*, std::string*)>& type,
                        bool aisKnown) {
    if (saved_.tabs.empty()) return;
    std::map<std::string, std::size_t> ids;
    const auto activeId = model.tab_count() ? model.tab(model.active()).id : ~std::size_t{};
    for (std::size_t i = 0; i < model.tab_count(); ++i) ids[keyOf(model.tab(i))] = model.tab(i).id;
    // A layout saved when there was one dashboard keeps it as "sprint": it is the first dashboard the layout does not
    // also keep by name, until the next save keeps that one by its name.
    for (std::size_t i = 0; i < model.tab_count() && !ids.contains(std::string(kOldSprint)); ++i) {
        const Tab& tab = model.tab(i);
        if (tab.kind != TabKind::Dashboard) continue;
        const std::string named = keyOf(tab);
        if (std::none_of(saved_.tabs.begin(), saved_.tabs.end(), [&](const auto& row) { return row.key == named; }))
            ids[std::string(kOldSprint)] = tab.id;
    }
    std::set<std::string> opened;
    for (const auto& [id, key] : localKeys_) opened.insert(key);
    std::vector<std::pair<std::size_t, std::string>> order;
    for (const auto& row : saved_.tabs) {
        if (!opened.contains(row.key) && row.key.starts_with("extension:")) {
            std::string group, title;
            if (type(row.detail, &group, &title)) {
                const auto id = model.open_extension(row.detail, std::move(group), std::move(title));
                localKeys_[id] = row.key;
                ids[row.key] = id;
            }
        }
        if (auto found = ids.find(row.key); found != ids.end()) order.emplace_back(found->second, row.group);
    }
    model.restore_tab_layout(order);
    auto selected = ids.find(pending_);
    const auto wanted = selected != ids.end() ? selected->second : activeId;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).id == wanted && model.active() != i) model.select_tab(i);
    }
    // A saved AI's or dashboard's tab is waited for until the server's state is known (the dashboards come with its AIs).
    if (selected != ids.end() || pending_.starts_with("terminal:") ||
        (aisKnown && (pending_.starts_with("ai:") || pending_.starts_with(kDashboardKey) || pending_ == kOldSprint)))
        pending_.clear();
}

void TabLayout::capture(const Model& model, const std::vector<std::string>& directories, bool aisKnown) {
    SavedLayout next;
    std::set<std::string> live;
    std::size_t terminal = 0;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        const auto& tab = model.tab(i);
        std::string key = keyOf(tab);
        live.insert(key);
        if (i == model.active()) next.selected = key;
        std::string detail = tab.type;
        if (tab.kind == TabKind::Terminal) {
            detail = terminal < directories.size() ? directories[terminal] : "";
            ++terminal;
        } else if (tab.kind != TabKind::Extension) {
            detail.clear();
        }
        next.tabs.push_back({std::move(key), tab.group, std::move(detail)});
    }
    // Keep offline AIs and extensions whose registrations have not loaded yet at the same neighboring tab.
    // A local instance seen in this run and now absent was closed by the person.
    std::set<std::string> opened;
    for (const auto& [id, key] : localKeys_) opened.insert(key);
    std::map<std::string, std::vector<LayoutTab>> preceding;
    std::vector<LayoutTab> waiting;
    for (const auto& old : saved_.tabs) {
        if (live.contains(old.key)) {
            if (!waiting.empty()) preceding[old.key].swap(waiting);
        } else if ((old.key == "primary" && (!aisKnown || pending_ == old.key)) ||
                   (!aisKnown && (old.key.starts_with("ai:") || old.key.starts_with(kDashboardKey) ||
                                  old.key == kOldSprint)) ||
                   (!opened.contains(old.key) && old.key.starts_with("extension:"))) {
            waiting.push_back(old);
        }
    }
    if (!preceding.empty()) {
        std::vector<LayoutTab> merged;
        for (auto& row : next.tabs) {
            if (const auto before = preceding.find(row.key); before != preceding.end())
                merged.insert(merged.end(), before->second.begin(), before->second.end());
            merged.push_back(std::move(row));
        }
        next.tabs = std::move(merged);
    }
    next.tabs.insert(next.tabs.end(), waiting.begin(), waiting.end());
    std::erase_if(localKeys_, [&](const auto& entry) { return !live.contains(entry.second); });
    if (!pending_.empty()) next.selected = pending_;
    saved_ = std::move(next);
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

std::vector<AIInfo> loadTabOrder(const TabOrderStore& store, const std::vector<AIInfo>& loaded,
        std::string* warning) {
    return applyTabOrder(loaded, loadTabOrderNames(store, warning));
}

bool saveTabOrder(const TabOrderStore& store, const Model& model, std::string* error) {
    std::string why;
    if (store.write(formatTabOrder(aiTabNames(model)), &why))
        return true;
    *error = "the tab order could not be saved to " + store.where + ": " + why;
    return false;
}

std::string formatSelectedTab(const Model& model) {
    std::string text(kSelectedHeader);
    text += '\n';
    const Tab* tab = model.tab_count() != 0 ? &model.tab(model.active()) : nullptr;
    if (tab == nullptr || tab->kind == TabKind::Extension) {
        text += kFirstWord;
    } else if (tab->kind == TabKind::Dashboard) {
        text += std::string(kDashboardPrefix) + tab->name;
    } else if (tab->kind == TabKind::Terminal) {
        std::size_t place = 0;
        for (std::size_t i = 0; i <= model.active(); ++i)
            place += model.tab(i).kind == TabKind::Terminal ? 1 : 0;
        text += std::string(kTerminalPrefix) + std::to_string(place);
    } else {
        text += std::string(kAIPrefix) + tab->name;
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
    if (header != kSelectedHeader && header != kSelectedHeader2 && header != kSelectedHeader1) {
        *error = "its first line is not \"" + std::string(kSelectedHeader) + "\"";
        return false;
    }
    std::string_view line = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    line = line.substr(0, line.find('\n'));
    if (line.empty()) {
        *error = "it names no tab";
        return false;
    }
    // The first dashboard, or the first tab: "first", or the one dashboard's "sprint" of version 1 or 2.
    if ((header == kSelectedHeader && line == kFirstWord) || (header != kSelectedHeader && line == kOldSprint))
        return true;
    if (header == kSelectedHeader && line.substr(0, kDashboardPrefix.size()) == kDashboardPrefix) {
        const std::string name(line.substr(kDashboardPrefix.size()));
        if (std::string problem = sync::dashboardNameProblem(name); !problem.empty()) {
            *error = "its dashboard line names no dashboard (" + problem + ")";
            return false;
        }
        saved->name = name;
        return true;
    }
    if (header == kSelectedHeader1 && line == kTerminalWord) {
        saved->kind = TabKind::Terminal;
        return true;
    }
    if (header != kSelectedHeader1 && line.substr(0, kTerminalPrefix.size()) == kTerminalPrefix) {
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
    for (const std::string_view prefix : {kAIPrefix, kOldAIPrefix}) {
        if (line.substr(0, prefix.size()) != prefix) continue;
        if (line.size() == prefix.size()) {
            *error = "its ai line has no name";
            return false;
        }
        saved->kind = TabKind::AI;
        saved->name = std::string(line.substr(prefix.size()));
        return true;
    }
    *error = "its second line \"" + std::string(line) + "\" is not first, dashboard <name>, terminal <n> or ai <name>";
    return false;
}

std::size_t savedTabPosition(const Model& model, const SavedTab& saved) {
    std::size_t terminals = 0;
    std::size_t first = 0;
    bool dashboard = false;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        const Tab& tab = model.tab(i);
        if (tab.kind == TabKind::Dashboard && !dashboard) {
            first = i;
            dashboard = true;
        }
        if (saved.kind == TabKind::Dashboard && tab.kind == TabKind::Dashboard && !saved.name.empty() &&
            tab.name == saved.name)
            return i;
        if (saved.kind == TabKind::AI && tab.kind == TabKind::AI && tab.name == saved.name)
            return i;
        if (saved.kind == TabKind::Terminal && tab.kind == TabKind::Terminal && ++terminals == saved.terminal)
            return i;
    }
    return first;
}

SavedTab loadSavedTab(const TabOrderStore& store, std::string* warning, bool* loaded) {
    warning->clear();
    if (loaded) *loaded = false;
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
    if (loaded) *loaded = true;
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

int loadTextScale(const TabOrderStore& store, int lo, int hi, std::string* warning, int unsaved) {
    warning->clear();
    std::string text;
    std::string error;
    int steps = 0;
    if (!store.read(&text, &error)) {
        if (!error.empty())
            *warning = "the saved text scale " + store.where + " is not used: " + error +
                "; deleting it resets the scale";
        return unsaved;
    }
    if (!parseTextScale(text, &steps, &error)) {
        *warning = "the saved text scale " + store.where + " is not used: " + error + "; deleting it resets the scale";
        return unsaved;
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
            return refuse("a line is not draft <server> <ai> <bytes>");
        }
        const std::string_view server = line.substr(kDraftPrefix.size(), first - kDraftPrefix.size());
        const std::string_view name = line.substr(first + 1, last - first - 1);
        const std::string_view digits = line.substr(last + 1);
        if (!keptServer(server) || !accounts::isName(name)) {
            return refuse("a line is not draft <server> <ai> <bytes>");
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
            return refuse("it keeps two drafts for one AI of one server");
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
