# build_unit_lto: the record CMakeLists.txt writes after adding Prax (prax-ipo.txt: NEXUS_LTO=<value>, then each Prax
# target and its INTERPROCEDURAL_OPTIMIZATION_RELEASE) agrees with the option. With NEXUS_LTO off no Prax target may keep
# whole-program optimisation; with it on, Prax's own setting stands, whatever it is (none in a Debug configure, where
# Prax sets none).
#   cmake -DRECORD=<build>/prax-ipo.txt -P lto_check.cmake
file(STRINGS "${RECORD}" lines)
list(POP_FRONT lines first)
if(NOT first MATCHES "^NEXUS_LTO=(.*)$")
    message(FATAL_ERROR "${RECORD} does not start with NEXUS_LTO=")
endif()
set(lto "${CMAKE_MATCH_1}")
set(on "")
set(count 0)
foreach(line IN LISTS lines)
    math(EXPR count "${count} + 1")
    if(line MATCHES "^([^ ]+) (ON|TRUE|1)$")
        list(APPEND on "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(count EQUAL 0)
    message(FATAL_ERROR "${RECORD} names no Prax target")
endif()
list(LENGTH on kept)
if(NOT lto AND NOT kept EQUAL 0)
    message(FATAL_ERROR "NEXUS_LTO is off, and ${kept} of ${count} Prax targets keep whole-program optimisation: ${on}")
endif()
message(STATUS "NEXUS_LTO=${lto}: ${kept} of ${count} Prax targets with whole-program optimisation")
