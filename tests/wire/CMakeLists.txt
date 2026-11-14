# Tests of nexus's formats as schema's variable tables (schema/Nexus.schema, generated into schema/gen), label unit:
#   wire_test   every table written and read back; and this version and a later one (next/NexusNext.schema, generated
#               into next/gen, test only) reading each other's bytes both ways. In memory: no time, no sockets, no
#               files. One ctest entry per group, so they run in parallel.

# The later version's generated code, a library of its own so that its translation units never meet this version's.
add_library(nexus_wire_next STATIC next/gen/NexusNextTable.cpp next/gen/NexuswirenextView.cpp)
target_include_directories(nexus_wire_next PUBLIC next/gen)
target_compile_options(nexus_wire_next PRIVATE ${NEXUS_WARNINGS})

# wire_test.cpp includes only this version's headers (NexusTable.h), wire_next.cpp only the later one's
# (NexusNextTable.h); each version's code is in its own namespace.
add_executable(wire_test wire_test.cpp wire_next.cpp)
target_link_libraries(wire_test PRIVATE nexus_wire nexus_wire_next)
target_compile_options(wire_test PRIVATE ${NEXUS_WARNINGS})

# It runs under the sanitizers with the model's programs (cmake --build <dir> --target model_checks).
add_dependencies(model_checks wire_test)

foreach(group roundtrip versions)
    add_test(NAME wire_unit_${group} COMMAND wire_test ${group})
    set_tests_properties(wire_unit_${group} PROPERTIES LABELS unit TIMEOUT 60)
endforeach()
