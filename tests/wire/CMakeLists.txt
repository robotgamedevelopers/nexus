# Tests of nexus's formats as schema's variable tables (schema/Nexus.schema, generated into schema/gen), label unit:
#   wire_test   every table written and read back; and this version and a later one (next/NexusNext.schema, generated
#               into next/gen, test only) reading each other's bytes both ways. In memory: no time, no sockets, no
#               files. One ctest entry per group, so they run in parallel. The sanitizer jobs build it beside the model
#               checks (--target model_checks wire_test) and run it.
#   nexus_versions_test
#               the same both ways through nexus's real wire code: the sessions diff, the actions and the Hello.

# The later version's generated code, a library of its own so that its translation units never meet this version's.
# MSVC's C4305 is off in its generated sources, as in this version's (src/wire/tables.h says why).
set(next_sources next/gen/NexusNextTable.cpp next/gen/NexuswirenextView.cpp)
add_library(nexus_wire_next STATIC ${next_sources})
target_include_directories(nexus_wire_next PUBLIC next/gen)
target_compile_options(nexus_wire_next PRIVATE ${NEXUS_WARNINGS})
if(MSVC)
    set_source_files_properties(${next_sources} PROPERTIES COMPILE_OPTIONS /wd4305)
endif()

# The later version's side of both tests (versions.h), compiled once: wire_next.cpp includes only the later version's
# headers (NexusNextTable.h), and the tests only this version's; each version's code is in its own namespace.
add_library(nexus_wire_next_side STATIC wire_next.cpp)
target_link_libraries(nexus_wire_next_side PUBLIC nexus_wire_next)
target_compile_options(nexus_wire_next_side PRIVATE ${NEXUS_WARNINGS})

add_executable(wire_test wire_test.cpp)
target_link_libraries(wire_test PRIVATE nexus_wire nexus_wire_next_side)
target_compile_options(wire_test PRIVATE ${NEXUS_WARNINGS})

foreach(group roundtrip versions)
    add_test(NAME wire_unit_${group} COMMAND wire_test ${group})
    set_tests_properties(wire_unit_${group} PROPERTIES LABELS unit TIMEOUT 60)
endforeach()

# The earlier version, state format 4 (prev/NexusPrev.schema, generated into prev/gen, test only: nexus before the
# in-app sign-in), the same way: its generated code and its side (wire_prev.cpp), each in its own library.
set(prev_sources prev/gen/NexusPrevTable.cpp prev/gen/NexuswireprevView.cpp)
add_library(nexus_wire_prev STATIC ${prev_sources})
target_include_directories(nexus_wire_prev PUBLIC prev/gen)
target_compile_options(nexus_wire_prev PRIVATE ${NEXUS_WARNINGS})
if(MSVC)
    set_source_files_properties(${prev_sources} PROPERTIES COMPILE_OPTIONS /wd4305)
endif()
add_library(nexus_wire_prev_side STATIC wire_prev.cpp)
target_link_libraries(nexus_wire_prev_side PUBLIC nexus_wire_prev)
target_compile_options(nexus_wire_prev_side PRIVATE ${NEXUS_WARNINGS})

# Main as it shipped at format 5 before sign-in by a pasted code (main5/NexusMain.schema, generated into main5/gen,
# test only), the same way.
set(main_sources main5/gen/NexusMainTable.cpp main5/gen/NexuswiremainView.cpp)
add_library(nexus_wire_main STATIC ${main_sources})
target_include_directories(nexus_wire_main PUBLIC main5/gen)
target_compile_options(nexus_wire_main PRIVATE ${NEXUS_WARNINGS})
if(MSVC)
    set_source_files_properties(${main_sources} PROPERTIES COMPILE_OPTIONS /wd4305)
endif()
add_library(nexus_wire_main_side STATIC wire_main.cpp)
target_link_libraries(nexus_wire_main_side PUBLIC nexus_wire_main)
target_compile_options(nexus_wire_main_side PRIVATE ${NEXUS_WARNINGS})

# nexus's own wire code (sync::Sessions, sync::Actions, the Hello) against the later version's generated code, the
# earlier one's (format 4) and main's format 5, both ways: nexus_versions_test.cpp includes only nexus's headers.
add_executable(nexus_versions_test nexus_versions_test.cpp)
target_link_libraries(nexus_versions_test PRIVATE nexus_sync nexus_wire_next_side nexus_wire_prev_side
    nexus_wire_main_side)
target_compile_options(nexus_versions_test PRIVATE ${NEXUS_WARNINGS})

foreach(group sessions bus actions hello signin paste)
    add_test(NAME wire_unit_nexus_${group} COMMAND nexus_versions_test ${group})
    set_tests_properties(wire_unit_nexus_${group} PROPERTIES LABELS unit TIMEOUT 60)
endforeach()
