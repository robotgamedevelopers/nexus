// Format 4 diffs sync_test crafts as schema's tables, which nexus's own code would never write (crafted_tables.h). In
// a translation unit of their own, so that only this one of sync_test's includes the generated header and the rest
// compiles beside it.

#include "crafted_tables.h"

#include <cstdint>

#include "wire/tables.h"

namespace synctest {

namespace {

std::string saved(const nexuswire::SessionsDiffBuilder& w) {
    std::string bytes(static_cast<std::size_t>(nexuswire::SessionsDiffMeasure(w)), '\0');
    const std::int64_t wrote = nexuswire::SessionsDiffSave(w, reinterpret_cast<std::uint8_t*>(bytes.data()),
        static_cast<std::int64_t>(bytes.size()));
    if (wrote != static_cast<std::int64_t>(bytes.size())) {
        return std::string();
    }
    return bytes;
}

} // namespace

std::string craftedOverflowDiff() {
    nexuswire::SessionsDiffBuilder w;
    nexuswire::SessionChange* s = nexuswire::SessionsDiffSessionsAdd(w.main, w.GetRoot()->sessions);
    s->id = 1;
    s->changed = true;
    s->first = ~std::uint64_t(0);
    s->keep_from = ~std::uint64_t(0);
    nexuswire::EntryChange* e = nexuswire::SessionChangeEntriesAdd(w.main, s->entries);
    e->kind = nexuswire::EntryKind::Person;
    e->closed = true;
    s->queue_changed = true;
    return saved(w);
}

std::string craftedUnsetKindDiff() {
    nexuswire::SessionsDiffBuilder w;
    nexuswire::SessionChange* s = nexuswire::SessionsDiffSessionsAdd(w.main, w.GetRoot()->sessions);
    s->id = 1;
    s->changed = true;
    nexuswire::EntryChange* e = nexuswire::SessionChangeEntriesAdd(w.main, s->entries);
    e->closed = true;
    nexuswire::TableStringEmplace(w.main, e->tail, "a later kind", 12);
    s->queue_changed = true;
    return saved(w);
}

} // namespace synctest
