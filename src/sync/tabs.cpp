#include "sync/tabs.h"

#include <algorithm>
#include <stdexcept>

#include "sync/wire.h"

namespace nexus::sync {

namespace {

bool sameEntry(const EntryRef& a, const EntryRef& b) {
    return a == b || *a == *b;
}

// EntryKind on the wire: Person 0, Friend 1, Notice 2.
bool validKind(std::uint8_t k) {
    return k <= static_cast<std::uint8_t>(EntryKind::Notice);
}

// The entry `tab` holds at transcript index i, or null.
const EntryRef* entryAt(const TabView* tab, std::uint64_t i) {
    if (tab == nullptr || i < tab->first || i >= tab->total())
        return nullptr;
    return &tab->entries[static_cast<std::size_t>(i - tab->first)];
}

std::vector<TabRef>::const_iterator lookup(const std::vector<TabRef>& tabs, const std::string& name) {
    auto it = std::lower_bound(tabs.begin(), tabs.end(), name,
        [](const TabRef& t, const std::string& n) { return t->name < n; });
    return it != tabs.end() && (*it)->name == name ? it : tabs.end();
}

// One changed tab: its first index; the index k of the first entry `a` does not hold as it is; the entries from k to
// the end, each its kind, closed flag, the length of text `a` holds of it (non-zero only for an entry `a` holds open
// that only grew) and the rest of its text; then a byte, 1 if the queue changed, and if so the queue.
void writeTab(Writer& w, const TabView& b, const TabView* a) {
    w.u64(b.first);
    std::uint64_t k = b.first;
    while (k < b.total()) {
        const EntryRef* had = entryAt(a, k);
        if (had == nullptr || !sameEntry(*had, b.entries[static_cast<std::size_t>(k - b.first)]))
            break;
        ++k;
    }
    w.u64(k);
    w.u64(b.total() - k);
    for (std::uint64_t i = k; i < b.total(); ++i) {
        const TabEntry& e = *b.entries[static_cast<std::size_t>(i - b.first)];
        const EntryRef* had = entryAt(a, i);
        std::size_t offset = 0;
        if (had != nullptr && !(*had)->closed && (*had)->kind == e.kind && (*had)->text.size() <= e.text.size() &&
                e.text.compare(0, (*had)->text.size(), (*had)->text) == 0)
            offset = (*had)->text.size();
        w.byte(static_cast<std::uint8_t>(e.kind));
        w.byte(e.closed ? 1 : 0);
        w.u64(offset);
        w.u64(e.text.size() - offset);
        w.raw(e.text.data() + offset, e.text.size() - offset);
    }
    const bool queueChanged = a == nullptr || a->queue != b.queue;
    w.byte(queueChanged ? 1 : 0);
    if (queueChanged) {
        w.u64(b.queue.size());
        for (const std::string& q : b.queue)
            w.str(q);
    }
}

TabRef readTab(Reader& r, std::size_t left, std::string name, const TabView* a) {
    auto t = std::make_shared<TabView>();
    t->name = std::move(name);
    t->first = r.u64();
    const std::uint64_t k = r.u64();
    const std::uint64_t count = r.u64();
    if (k < t->first)
        throw BadDiff("tab " + t->name + ": diff sends from entry " + std::to_string(k) + ", before its tail's first, " +
            std::to_string(t->first));
    // Each entry takes at least four bytes, so a count past what is left of the diff is malformed.
    if (count > left)
        throw BadDiff("tab " + t->name + ": diff says it sends " + std::to_string(count) + " entries in " +
            std::to_string(left) + " bytes");
    if (k - t->first > (a == nullptr ? 0 : a->entries.size()))
        throw BadDiff("tab " + t->name + ": diff keeps " + std::to_string(k - t->first) + " entries this state lacks");
    t->entries.reserve(static_cast<std::size_t>(k - t->first + count));
    for (std::uint64_t i = t->first; i < k; ++i) {
        const EntryRef* had = entryAt(a, i);
        if (had == nullptr)
            throw BadDiff("tab " + t->name + ": diff keeps entry " + std::to_string(i) + ", which this state lacks");
        t->entries.push_back(*had);
    }
    for (std::uint64_t i = k; i < k + count; ++i) {
        const std::uint8_t kind = r.byte();
        if (!validKind(kind))
            throw BadDiff("tab " + t->name + ": entry " + std::to_string(i) + " has kind " + std::to_string(kind));
        const std::uint8_t closed = r.byte();
        if (closed > 1)
            throw BadDiff("tab " + t->name + ": entry " + std::to_string(i) + " has closed flag " +
                std::to_string(closed));
        const std::uint64_t offset = r.u64();
        std::string text = r.str();
        if (offset != 0) {
            const EntryRef* had = entryAt(a, i);
            if (had == nullptr || (*had)->closed || static_cast<std::uint8_t>((*had)->kind) != kind ||
                    (*had)->text.size() != offset)
                throw BadDiff("tab " + t->name + ": entry " + std::to_string(i) + " grows from " +
                    std::to_string(offset) + " bytes, which this state does not hold open");
            text = (*had)->text + text;
        }
        t->entries.push_back(std::make_shared<const TabEntry>(
            TabEntry{static_cast<EntryKind>(kind), std::move(text), closed == 1}));
    }
    const std::uint8_t queueChanged = r.byte();
    if (queueChanged == 0) {
        if (a == nullptr)
            throw BadDiff("tab " + t->name + ": diff keeps the queue of a tab this state lacks");
        t->queue = a->queue;
    } else if (queueChanged == 1) {
        const std::uint64_t n = r.u64();
        if (n > left)
            throw BadDiff("tab " + t->name + ": diff says its queue holds " + std::to_string(n) + " prompts in " +
                std::to_string(left) + " bytes");
        for (std::uint64_t q = 0; q < n; ++q)
            t->queue.push_back(r.str());
    } else {
        throw BadDiff("tab " + t->name + ": queue flag " + std::to_string(queueChanged) + ", neither 0 nor 1");
    }
    return t;
}

} // namespace

bool TabView::operator==(const TabView& x) const {
    if (name != x.name || first != x.first || entries.size() != x.entries.size() || queue != x.queue)
        return false;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!sameEntry(entries[i], x.entries[i]))
            return false;
    }
    return true;
}

TabView tailOf(std::string name, const std::vector<EntryRef>& transcript, std::vector<std::string> queue, Tail tail,
               EntryRef open) {
    TabView t;
    t.name = std::move(name);
    t.queue = std::move(queue);
    // The tail of the closed entries, then the open entry, if any, which the tail's rules do not count: so a state
    // always carries the newest closed entry, and a window that missed entries always gets one after its notice.
    const std::size_t count = transcript.size();
    std::size_t from = count;
    std::size_t bytes = 0;
    while (from > 0) {
        const std::size_t size = transcript[from - 1]->text.size();
        const bool newest = from == count;
        if (!newest && (count - from >= tail.entries || bytes + size > tail.bytes)) {
            break;
        }
        bytes += size;
        --from;
    }
    t.first = from;
    for (std::size_t i = from; i < count; ++i) {
        t.entries.push_back(transcript[i]);
    }
    if (open) {
        t.entries.push_back(std::move(open));
    }
    return t;
}

Tabs::Tabs() : tabs_(std::make_shared<const std::vector<TabRef>>()) {}

Tabs::Tabs(std::vector<TabRef> tabs) {
    for (std::size_t i = 1; i < tabs.size(); ++i) {
        if (!(tabs[i - 1]->name < tabs[i]->name))
            throw std::logic_error("Tabs: tabs out of name order or named twice: " + tabs[i - 1]->name + ", " +
                tabs[i]->name);
    }
    tabs_ = std::make_shared<const std::vector<TabRef>>(std::move(tabs));
}

const TabView* Tabs::find(const std::string& name) const {
    auto it = lookup(*tabs_, name);
    return it == tabs_->end() ? nullptr : it->get();
}

// A diff: the number of tabs; then for each, in name order, its name and a byte: 0 when `existing` holds it as it is,
// else 1 and the tab (writeTab). A tab `existing` holds and this state lacks is not named, which removes it.
std::string Tabs::diff_from(const Tabs& existing) const {
    if (*this == existing)
        return std::string();
    Writer w;
    w.u64(tabs_->size());
    for (const TabRef& b : *tabs_) {
        w.str(b->name);
        const TabView* a = existing.find(b->name);
        if (a != nullptr && (a == b.get() || *a == *b)) {
            w.byte(0);
            continue;
        }
        w.byte(1);
        writeTab(w, *b, a);
    }
    return w.take();
}

std::string Tabs::init_diff() const {
    return diff_from(Tabs());
}

void Tabs::apply_string(const std::string& diff) {
    if (diff.empty())
        return;
    Reader r(diff);
    const std::uint64_t n = r.u64();
    if (n > diff.size())
        throw BadDiff("tabs diff says it holds " + std::to_string(n) + " tabs in " + std::to_string(diff.size()) +
            " bytes");
    std::vector<TabRef> tabs;
    tabs.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
        std::string name = r.str();
        if (!tabs.empty() && !(tabs.back()->name < name))
            throw BadDiff("tabs diff names tab " + name + " out of order or twice");
        const std::uint8_t changed = r.byte();
        auto had = lookup(*tabs_, name);
        const TabView* a = had == tabs_->end() ? nullptr : had->get();
        if (changed == 0) {
            if (a == nullptr)
                throw BadDiff("tabs diff keeps tab " + name + ", which this state lacks");
            tabs.push_back(*had);
        } else if (changed == 1) {
            tabs.push_back(readTab(r, diff.size(), std::move(name), a));
        } else {
            throw BadDiff("tabs diff marks tab " + name + " with " + std::to_string(changed) + ", neither 0 nor 1");
        }
    }
    r.end();
    tabs_ = std::make_shared<const std::vector<TabRef>>(std::move(tabs));
}

bool Tabs::operator==(const Tabs& x) const {
    if (tabs_ == x.tabs_)
        return true;
    if (tabs_->size() != x.tabs_->size())
        return false;
    for (std::size_t i = 0; i < tabs_->size(); ++i) {
        const TabRef& a = (*tabs_)[i];
        const TabRef& b = (*x.tabs_)[i];
        if (a != b && !(*a == *b))
            return false;
    }
    return true;
}

} // namespace nexus::sync
