# schema (github.com/mas-bandwidth/schema): the compiler that turns schema/Nexus.schema into the C++ of nexus's formats
# across versions (docs/formats.md). Pinned to one exact commit of its main, as Prax is: its table wire is pre-release
# until schema 3.0.0 and may change in a minor version, so nexus moves the pin on purpose, never by following a branch.
# The compiler is AGPL-3.0 with an exception that makes its output ours under any terms; the generated files say so.
#
# The generated code is checked in (schema/gen, and tests/wire/next/gen and tests/wire/prev/gen for the tests' later
# and earlier versions), so a build needs neither Go nor schema. This file does two things:
#   include(cmake/schema.cmake)          from CMakeLists.txt: the library nexus_wire over schema/gen, and the target
#                                        schema_generate, which runs this file as below
#   cmake -P cmake/schema.cmake          regenerates both trees at the pin: fetches the pinned commit alone (no history)
#                                        into SCHEMA_CACHE, builds the compiler with Go once per pin, checks each unit
#                                        (schema/tables.baseline refuses an edit that changes what stored data means),
#                                        and writes the code in place of the old. CI runs it and fails on any
#                                        difference from what is checked in (.github/workflows/ci.yml, schema-generated).
#     -DSCHEMA_CACHE=<dir>               where the pinned sources and the compiler are kept (default: build/schema)
#
# Go comes from PATH, or ~/sdk/bin as the fleet installs it.

set(NEXUS_SCHEMA_REPOSITORY https://github.com/mas-bandwidth/schema.git)
set(NEXUS_SCHEMA_COMMIT c3ad228767db2073411099d7ee635a0fd38b4190)

get_filename_component(nexus_schema_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    # The formats as schema's variable tables (schema/Nexus.schema). No OS, no time, no Prax. nexus includes them
    # through src/wire/tables.h, which says why MSVC's C4305 is off across the generated code; the generated sources
    # here are compiled with it off for the same reason, and nothing else is.
    set(nexus_wire_sources ${nexus_schema_root}/schema/gen/NexusTable.cpp
        ${nexus_schema_root}/schema/gen/NexuswireView.cpp)
    add_library(nexus_wire STATIC ${nexus_wire_sources})
    target_include_directories(nexus_wire PUBLIC ${nexus_schema_root}/schema/gen ${nexus_schema_root}/src)
    target_compile_options(nexus_wire PRIVATE ${NEXUS_WARNINGS})
    if(MSVC)
        set_source_files_properties(${nexus_wire_sources} PROPERTIES COMPILE_OPTIONS /wd4305)
    endif()

    add_custom_target(schema_generate
        COMMAND ${CMAKE_COMMAND} -DSCHEMA_CACHE=${CMAKE_BINARY_DIR}/schema -P ${CMAKE_CURRENT_LIST_FILE}
        WORKING_DIRECTORY ${nexus_schema_root}
        COMMENT "Regenerating schema/gen, tests/wire/next/gen and tests/wire/prev/gen at schema ${NEXUS_SCHEMA_COMMIT}"
        VERBATIM)
    return()
endif()

# ---- cmake -P cmake/schema.cmake: regenerate at the pin ----

if(NOT SCHEMA_CACHE)
    set(SCHEMA_CACHE "${nexus_schema_root}/build/schema")
endif()
string(SUBSTRING "${NEXUS_SCHEMA_COMMIT}" 0 12 nexus_schema_short)
set(pin_dir "${SCHEMA_CACHE}/${NEXUS_SCHEMA_COMMIT}")
set(compiler "${pin_dir}/schema${CMAKE_EXECUTABLE_SUFFIX}")

function(nexus_schema_run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "nexus: `${shown}` failed (${result}); see its output above")
    endif()
endfunction()

if(NOT EXISTS "${compiler}")
    find_program(nexus_git git REQUIRED)
    find_program(nexus_go go HINTS "$ENV{HOME}/sdk/bin")
    if(NOT nexus_go)
        message(FATAL_ERROR "nexus: regenerating the schema code needs Go (1.26 or later) on PATH or in ~/sdk/bin; "
            "install it, or leave schema/gen as it is checked in")
    endif()
    set(src "${pin_dir}/src")
    file(REMOVE_RECURSE "${src}")
    file(MAKE_DIRECTORY "${src}")
    nexus_schema_run(${nexus_git} -C "${src}" init -q)
    nexus_schema_run(${nexus_git} -C "${src}" fetch -q --depth 1 ${NEXUS_SCHEMA_REPOSITORY} ${NEXUS_SCHEMA_COMMIT})
    nexus_schema_run(${nexus_git} -C "${src}" checkout -q FETCH_HEAD)
    execute_process(COMMAND ${CMAKE_COMMAND} -E chdir "${src}" ${nexus_go} build -o "${compiler}.tmp" ./cmd/schema
        RESULT_VARIABLE built)
    if(NOT built EQUAL 0)
        message(FATAL_ERROR "nexus: building schema's compiler at ${NEXUS_SCHEMA_COMMIT} with ${nexus_go} failed "
            "(${built}); see its output above")
    endif()
    file(RENAME "${compiler}.tmp" "${compiler}")
    file(REMOVE_RECURSE "${src}")    # the compiler is all that is kept
endif()

# The compiler states the commit it was built from; one built from anything but the pin is refused.
execute_process(COMMAND "${compiler}" version OUTPUT_VARIABLE version RESULT_VARIABLE result
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(FIND "${version}" "${nexus_schema_short}" at)
if(NOT result EQUAL 0 OR at EQUAL -1)
    message(FATAL_ERROR "nexus: ${compiler} says '${version}', not the pinned commit ${nexus_schema_short}; remove "
        "${pin_dir} and run this again")
endif()

# Each unit, checked, then generated into a directory emptied first, so that a file the compiler no longer writes goes.
foreach(unit schema tests/wire/next tests/wire/prev)
    nexus_schema_run("${compiler}" check "${nexus_schema_root}/${unit}")
    file(REMOVE_RECURSE "${nexus_schema_root}/${unit}/gen")
    nexus_schema_run("${compiler}" generate --lang cpp --out "${nexus_schema_root}/${unit}/gen"
        "${nexus_schema_root}/${unit}")
endforeach()

# The tests' later version must differ from schema/Nexus.schema only by edits old data reads through: checked against
# schema/tables.baseline, under the later version's package name.
set(probe "${pin_dir}/next-against-baseline")
file(REMOVE_RECURSE "${probe}")
file(MAKE_DIRECTORY "${probe}")
file(COPY "${nexus_schema_root}/tests/wire/next/NexusNext.schema" DESTINATION "${probe}")
file(READ "${nexus_schema_root}/schema/tables.baseline" baseline)
string(REPLACE "\npackage nexuswire\n" "\npackage nexuswirenext\n" baseline "${baseline}")
file(WRITE "${probe}/tables.baseline" "${baseline}")
nexus_schema_run("${compiler}" check "${probe}")

message(STATUS "nexus: schema/gen, tests/wire/next/gen and tests/wire/prev/gen regenerated by ${version}")
