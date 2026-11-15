// Format 4 sessions diffs that sync_test crafts as schema's tables (crafted_tables.cpp); empty if one could not be
// written.

#pragma once

#include <string>

namespace synctest {

// Session 1 changed, its first and keep_from at the last uint64, one closed Person entry: well formed, and refused for
// numbering past the last entry number.
std::string craftedOverflowDiff();
// Session 1 changed, one closed entry whose writer set no kind (as a later version's kind reads), text "a later kind".
std::string craftedUnsetKindDiff();

} // namespace synctest
