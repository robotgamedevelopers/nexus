// The order of the friend tabs, the selected tab and the text scale, kept across launches: the person's drag, choice and
// size are remembered.
//
// Kept on this machine for now, in one small file under the user's application-support directory. Later this state
// moves to the server, with the rest of what the window shows.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "model.h"

namespace nexus {

// The friends in `loaded` (the store's order), in the order `saved` names them; friends `saved` does not name follow,
// in their loaded order. Saved names that no longer exist are dropped, and a name saved twice counts once. Pure.
std::vector<FriendInfo> applyTabOrder(const std::vector<FriendInfo>& loaded, const std::vector<std::string>& saved);

// The names of the friend tabs, in the order they stand. Terminal tabs are skipped: they are never saved. A saved
// order is applied to the friend list before the model is built (applyTabOrder), so it only ever reorders friends,
// inside the friend zone; terminals are added after.
std::vector<std::string> friendTabNames(const Model& model);

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

// The store in a file, tab-order.txt in nexus's own directory under the user's application-support directory
// (Prax's PrFileUserDirectory_Data). `where` is empty if the platform names no such directory.
TabOrderStore fileTabOrderStore();

// The store in a file at `path`, written atomically (Prax's prFileWrite: a file beside it, then a rename).
TabOrderStore fileTabOrderStore(std::string path);

// `loaded` in the saved order. On an unreadable or malformed file, `loaded` as it is, and `warning` gets one line naming
// the file, what was wrong and that deleting it resets the order.
std::vector<FriendInfo> loadTabOrder(const TabOrderStore& store, const std::vector<FriendInfo>& loaded,
    std::string* warning);

// Saves the order of the model's friend tabs. Returns false, with one line in `error` naming the file and the cause.
bool saveTabOrder(const TabOrderStore& store, const Model& model, std::string* error);

// The selected tab, kept across launches beside the order, by who it is and never by where it stands: the sprint tab, a
// friend by name, or a terminal (which does not survive a quit).
struct SavedTab {
    TabKind kind = TabKind::Sprint;
    std::string name;   // a friend's
};

// The file's text for the model's active tab: a header line, then "sprint", "terminal" or "friend <name>". Pure.
std::string formatSelectedTab(const Model& model);

// Reads the file's text into `saved`. Returns false, with `error` saying what is wrong, for text that is not a selected
// tab file. Pure.
bool parseSelectedTab(std::string_view text, SavedTab* saved, std::string* error);

// The position of the saved tab in `model`: the friend of that name, wherever it stands. The sprint tab (position 0)
// if the sprint tab or a terminal was saved, or the friend is no longer in the model. Pure.
std::size_t savedTabPosition(const Model& model, const SavedTab& saved);

// The store in a file, selected-tab.txt beside tab-order.txt. `where` is empty if the platform names no such directory.
TabOrderStore fileSelectedTabStore();

// The position to select at launch (savedTabPosition): position 0 if nothing is saved, and on an unreadable or malformed
// file, with `warning` one line naming the file, what was wrong and that deleting it resets the selection.
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

// The store in a file, text-scale.txt beside tab-order.txt. `where` is empty if the platform names no such directory.
TabOrderStore fileTextScaleStore();

// The steps to start at: 0 (the default scale) if nothing is saved, and on an unreadable or malformed file, with
// `warning` one line naming the file, what was wrong and that deleting it resets the scale. A saved number outside `lo`
// to `hi` is clamped into it, and `warning` says so.
int loadTextScale(const TabOrderStore& store, int lo, int hi, std::string* warning);

// Saves the steps. Returns false, with one line in `error` naming the file and the cause.
bool saveTextScale(const TabOrderStore& store, int steps, std::string* error);

} // namespace nexus
