// The order of the AI tabs, the selected tab, the text scale and the terminal tabs, kept across launches: the
// person's drag, choice, size and open terminals are remembered.
//
// Kept on this machine for now, each in one small file in the window's home, ~/.nexus/client (client_home.h). Later
// this state moves to the server, with the rest of what the window shows.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model.h"

namespace nexus {

// The AIs in `loaded` (the store's order), in the order `saved` names them; AIs `saved` does not name follow,
// in their loaded order. Saved names that no longer exist are dropped, and a name saved twice counts once. Pure.
std::vector<AIInfo> applyTabOrder(const std::vector<AIInfo>& loaded, const std::vector<std::string>& saved);

// The names of the AI tabs, in the order they stand. Terminal tabs are skipped: they are never saved. A saved
// order only ever orders AIs, inside the AI zone: the server's AIs are added to the model in it as they
// arrive (sync/client.h applyAIs), after any terminals were opened.
std::vector<std::string> aiTabNames(const Model& model);

// The file's text: a header line, then one name per line, UTF-8. Pure.
std::string formatTabOrder(const std::vector<std::string>& names);

// Reads the file's text into `names`. Returns false, with `error` saying what is wrong, for text that is not a tab
// order file. Pure.
bool parseTabOrder(std::string_view text, std::vector<std::string>* names, std::string* error);

// Where the order (or the selected tab, below) is kept: the seam that tests fill with an in-memory fake.
struct TabOrderStore {
    std::string where;          // the file's path, for messages
    // Writes the saved text to `text` and returns true; returns false with `error` empty if nothing is saved, or with
    // `error` saying why it could not be read.
    std::function<bool(std::string* text, std::string* error)> read;
    // Replaces the saved text, all or nothing. Returns false with `error` saying why.
    std::function<bool(const std::string& text, std::string* error)> write;
};

// The store in a file, tab-order.txt in the window's home, ~/.nexus/client (client_home.h), made mode 0700 at the first
// write. With no home (HOME, or USERPROFILE on Windows, unset), `where` is empty and both calls fail saying so.
TabOrderStore fileTabOrderStore();

// The store in a file at `path`, written atomically (Prax's prFileWrite: a file beside it, then a rename).
TabOrderStore fileTabOrderStore(std::string path);

// One atomic layout across tab kinds. AIs and the sprint board have intrinsic keys. Local instances receive
// persistent keys on creation so two terminals in the same directory and two tabs of the same type stay distinct.
struct LayoutTab {
    std::string key, group, detail;  // detail is a terminal's directory or an extension's registered type
    bool operator==(const LayoutTab&) const = default;
};
struct SavedLayout {
    std::string selected;
    std::vector<LayoutTab> tabs;
    bool operator==(const SavedLayout&) const = default;
};
TabOrderStore fileLayoutStore();
std::string formatLayout(const SavedLayout& layout);
bool parseLayout(std::string_view text, SavedLayout* layout);

class TabLayout {
public:
    explicit TabLayout(SavedLayout saved = {});
    const SavedLayout& saved() const { return saved_; }
    std::vector<std::pair<std::size_t, std::string>> reopenTerminals(Model& model);
    // Registers extensions only after their type becomes available. Restores order under the active fork's movement
    // rules and preserves focus by logical key, including an AI that arrives after startup.
    void restore(Model& model, const std::function<bool(const std::string&, std::string*, std::string*)>& type,
                 bool aisKnown);
    void personSelected() { pending_.clear(); }
    void capture(const Model& model, const std::vector<std::string>& directories, bool aisKnown);

private:
    std::string keyOf(const Tab& tab);
    SavedLayout saved_;
    std::string pending_;
    std::map<std::size_t, std::string> localKeys_;
    std::size_t serial_ = 0;
};

// The names the store saved, in order: none if nothing is saved, and on an unreadable or malformed file, with `warning`
// one line naming the file, what was wrong and that deleting it resets the order. The window reads them before it
// shows and orders the server's AIs by them as they arrive (sync/client.h applyAIs).
std::vector<std::string> loadTabOrderNames(const TabOrderStore& store, std::string* warning);

// `loaded` in the saved order: applyTabOrder over loadTabOrderNames.
std::vector<AIInfo> loadTabOrder(const TabOrderStore& store, const std::vector<AIInfo>& loaded,
    std::string* warning);

// Saves the order of the model's AI tabs. Returns false, with one line in `error` naming the file and the cause.
bool saveTabOrder(const TabOrderStore& store, const Model& model, std::string* error);

// The selected tab, kept across launches beside the order: the sprint tab, an AI by name, or a terminal by its place
// among the terminal tabs (which are reopened in their saved order, below).
struct SavedTab {
    TabKind kind = TabKind::Sprint;
    std::string name;           // an AI's
    std::size_t terminal = 0;   // a terminal's place among the terminal tabs, from 1; 0 for a version 1 file's
                                // "terminal", which named none
};

// The file's text for the model's active tab: a header line, then "sprint", "terminal <n>" (its place among the terminal
// tabs, from 1) or "ai <name>". Pure.
std::string formatSelectedTab(const Model& model);

// Reads the file's text into `saved`. Returns false, with `error` saying what is wrong, for text that is not a selected
// tab file. Reads version 1 files too, whose "terminal" names no terminal. Pure.
bool parseSelectedTab(std::string_view text, SavedTab* saved, std::string* error);

// The position of the saved tab in `model`: the AI of that name, wherever it stands, or the terminal at that place
// among the terminal tabs. The sprint tab (position 0) if the sprint tab was saved, or the tab saved is not in the
// model. Pure.
std::size_t savedTabPosition(const Model& model, const SavedTab& saved);

// The store in a file, selected-tab.txt beside tab-order.txt. `where` is empty if there is no home.
TabOrderStore fileSelectedTabStore();

// The tab the store saved: the sprint tab's SavedTab{} if nothing is saved, and on an unreadable or malformed file,
// with `warning` one line naming the file, what was wrong and that deleting it resets the selection. A saved AI is
// selected once the server's AIs have arrived (frame_host.h). When requested, loaded distinguishes a real saved
// choice (including the sprint tab) from the default.
SavedTab loadSavedTab(const TabOrderStore& store, std::string* warning, bool* loaded = nullptr);

// The position to select at launch: savedTabPosition of loadSavedTab.
std::size_t loadSelectedTab(const TabOrderStore& store, const Model& model, std::string* warning);

// Saves the model's active tab. Returns false, with one line in `error` naming the file and the cause.
bool saveSelectedTab(const TabOrderStore& store, const Model& model, std::string* error);

// The text scale (Command-plus and Command-minus), kept across launches beside the order: in steps from 1, as
// style::textScale takes it.
//
// The file's text: a header line, then the steps as a whole number, negative for smaller. Pure.
std::string formatTextScale(int steps);

// Reads the file's text into `steps`. Returns false, with `error` saying what is wrong, for text that is not a text
// scale file. Pure.
bool parseTextScale(std::string_view text, int* steps, std::string* error);

// The store in a file, text-scale.txt beside tab-order.txt. `where` is empty if there is no home.
TabOrderStore fileTextScaleStore();

// The steps to start at: `unsaved` (the platform's default, style.h kDefaultTextSteps) if nothing is saved, and on an
// unreadable or malformed file, with `warning` one line naming the file, what was wrong and that deleting it resets the
// scale. A saved number outside `lo` to `hi` is clamped into it, and `warning` says so.
int loadTextScale(const TabOrderStore& store, int lo, int hi, std::string* warning, int unsaved = 0);

// Saves the steps. Returns false, with one line in `error` naming the file and the cause.
bool saveTextScale(const TabOrderStore& store, int steps, std::string* error);

// The terminal tabs, kept across launches beside the order: one per terminal tab, in the order they stand, with the
// directory its shell worked in. At launch each is a new login shell in that directory; what the terminal showed and
// the programs running in it are not kept.
//
// The most terminals a launch reopens: a file that names more is cut to these, with a warning.
constexpr std::size_t kMaxSavedTerminals = 32;

// The directory of each terminal tab, in the order the tabs stand: `directoryOf(Tab::id)`, or empty (the home
// directory) where that is empty or holds a line feed or a NUL, which a line of the file cannot. Pure.
std::vector<std::string> terminalDirectories(const Model& model,
    const std::function<std::string(std::size_t id)>& directoryOf);

// The file's text: a header line, then one line per terminal tab, "terminal <directory>", or "terminal" for the home
// directory. Pure.
std::string formatTerminals(const std::vector<std::string>& directories);

// Reads the file's text into `directories`, empty for the home directory. Returns false, with `error` saying what is
// wrong, for text that is not a terminals file. Pure.
bool parseTerminals(std::string_view text, std::vector<std::string>* directories, std::string* error);

// The store in a file, terminals.txt beside tab-order.txt. `where` is empty if there is no home.
TabOrderStore fileTerminalsStore();

// The terminal tabs to reopen at launch, at most kMaxSavedTerminals: none if nothing is saved, and on an unreadable or
// malformed file, with `warning` one line naming the file, what was wrong and that deleting it reopens none. More than
// kMaxSavedTerminals are cut, and `warning` says so.
std::vector<std::string> loadTerminals(const TabOrderStore& store, std::string* warning);

// Saves the terminal tabs' directories. Returns false, with one line in `error` naming the file and the cause.
bool saveTerminals(const TabOrderStore& store, const std::vector<std::string>& directories, std::string* error);

// The drafts kept for AIs without a tab (sync/client.h KeptDrafts), kept across launches beside the order: what
// the person typed to an AI the server's list left out, until an AI of that name on the same server has a tab
// again. One file serves every window on this machine, each linked to its own server, so a draft is kept by server and
// AI name: the server is the window's link target, the host `--server` names as given (an alias and an address of
// one machine are two servers to it). A window with no server keeps none. The window never deletes a draft but the
// one it gives back; deleting the file deletes them all.
//
// The file is changed only by a change (KeptChange), read, merged and written whole under an OS lock on a file beside
// it (kept-drafts.lock), never by writing a window's own drafts over it: so two windows, each keeping or giving back
// drafts, never lose each other's (models/AIStrip.tla FileKeepsOthers). It is written 0600, by a rename.
using KeptFile = std::map<std::pair<std::string, std::string>, std::string>;    // (server, AI) -> draft

// A window's change of the file, for its server: the drafts it keeps (`put`, each AI's name and draft) and those it
// gave back to an AI's new tab (`taken`, the same). A draft put where the file holds another for that server and
// AI (another window's) is joined to it, kKeptJoin between, so that neither is lost; one given back leaves the file
// only while the file still holds exactly what was given (another window may have joined one to it since).
struct KeptChange {
    std::string server;
    std::vector<std::pair<std::string, std::string>> put;
    std::vector<std::pair<std::string, std::string>> taken;
};
constexpr std::string_view kKeptJoin = "\n\n";

// `change` applied to `file`, as above. Pure.
void applyKeptChange(KeptFile* file, const KeptChange& change);

// Whether `server` can key a kept draft: 1 to 255 bytes, none a space, a control character or DEL (a host as ssh takes
// it, user@ and all). A window whose server cannot keeps no draft across launches. Pure.
bool keptServer(std::string_view server);

// The drafts `file` keeps for `server`, by AI name. Pure.
std::map<std::string, std::string> keptFor(const KeptFile& file, const std::string& server);

// The file's text: a header line, then for each draft, in (server, AI) order, a line "draft <server> <ai>
// <bytes>", then that many bytes of the draft as typed (any bytes, newlines too), then a line feed. Pure.
std::string formatKeptDrafts(const KeptFile& file);

// Reads the file's text into `file`. Returns false, with `error` saying what is wrong, for text that is not a kept
// drafts file. Pure.
bool parseKeptDrafts(std::string_view text, KeptFile* file, std::string* error);

// Where the kept drafts are: the seam that tests fill with an in-memory fake.
struct KeptDraftsStore {
    std::string where;          // the file's path, for messages
    // The file as it is: true with `file` (empty when there is none yet), or false with `error` saying why it could
    // not be read or parsed.
    std::function<bool(KeptFile* file, std::string* error)> read;
    // Applies `change` under the lock: read, merge (applyKeptChange), write. Returns false, with `error` saying why,
    // and changes nothing, when the file cannot be locked, read, parsed or written.
    std::function<bool(const KeptChange& change, std::string* error)> update;
};

// The store in kept-drafts.txt beside tab-order.txt, locked by kept-drafts.lock there. `where` is empty if there is no
// home, and both calls fail saying so. Not on Windows, where the window has no server link: both calls fail there.
KeptDraftsStore fileKeptDraftsStore();
// The store in the file at `path`, locked by `path` with ".lock" in place of its ".txt" (or after it).
KeptDraftsStore fileKeptDraftsStore(std::string path);

// The drafts kept for `server`: none if nothing is saved, and on an unreadable or malformed file none, `readable`
// false and `warning` one line naming the file, what was wrong and that it is left as it is: the window then keeps
// this run's drafts in memory only and never changes the file.
std::map<std::string, std::string> loadKeptDrafts(const KeptDraftsStore& store, const std::string& server,
    bool* readable, std::string* warning);

// Applies a change to the file. Returns false, with one line in `error` naming the file and the cause.
bool saveKeptChange(const KeptDraftsStore& store, const KeptChange& change, std::string* error);

// The writer of the window's kept files: a thread of its own, so that a write to a slow or stalled disk never holds
// up a frame. A store it wraps hands each write to the thread and returns at once; of the writes to one store waiting
// at a time only the newest is made, since each replaces the whole file. A write that fails is logged on stderr with
// its cause (the store's own message, naming the file). The writer makes every waiting write before it ends.
class StoreWriter {
public:
    StoreWriter();
    ~StoreWriter();
    StoreWriter(const StoreWriter&) = delete;
    StoreWriter& operator=(const StoreWriter&) = delete;

    // `store` with its write handed to this thread. Reads stay where they are: they happen once, at launch, before
    // the window shows. The writer must outlive the store it returns.
    TabOrderStore wrap(TabOrderStore store);
    // `store` with its update handed to this thread: every change is made, in the order given, none dropped for a
    // newer one (each is a change, not the whole file). A change that fails is logged on stderr with its cause.
    KeptDraftsStore wrap(KeptDraftsStore store);
    // Waits until every write handed over so far is made.
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nexus
