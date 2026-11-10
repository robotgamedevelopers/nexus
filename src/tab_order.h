// The order of the friend tabs, kept across launches: the person's drag is remembered.
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

// The names of the friend tabs, in the order they stand. Every tab is a friend's today; when tabs of other kinds
// arrive (a terminal's, never saved), the one-line kind check goes in this function's loop (tab_order.cpp).
std::vector<std::string> friendTabNames(const Model& model);

// The file's text: a header line, then one name per line, UTF-8. Pure.
std::string formatTabOrder(const std::vector<std::string>& names);

// Reads the file's text into `names`. Returns false, with `error` saying what is wrong, for text that is not a tab
// order file. Pure.
bool parseTabOrder(std::string_view text, std::vector<std::string>* names, std::string* error);

// Where the order is kept: the seam that tests fill with an in-memory fake.
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

} // namespace nexus
