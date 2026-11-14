// The one way nexus includes the code schema generates from schema/Nexus.schema (schema/gen, cmake/schema.cmake): the
// tables of nexus's formats across versions, in namespace nexuswire.
//
// MSVC's C4305 is off across the generated header, and only there. At the pinned schema commit, the save of a table
// holding a list of unions (Record.changes []Change) measures each arm inside a function that returns bool, and that
// measure's failure paths are spelled `return -1;` (RecordSaveBodyFields and RecordSaveBodyFieldsRetain in
// NexusTable.h), which MSVC at /W4 reports as a truncation from int to bool. Clang and GCC compile it silently. It is
// the write side, whose checks schema's C++ leaves to the caller; the read side is not involved. Remove this when a
// pin generates `return false;` there.

#pragma once

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4305)
#endif

#include "NexusTable.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
