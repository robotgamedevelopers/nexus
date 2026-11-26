# What each test runs, recorded as it is registered, so that cmake/tiers.cmake can build the pull request tier's
# targets alone (tier_pr_build) from the same tier table that picks its tests (docs/ci-tiers.md).
#
# add_test is wrapped: it registers the test unchanged and records, by the test's name, its command and every target an
# argument names with $<TARGET_FILE:x>, $<TARGET_FILE_DIR:x>, $<TARGET_LINKER_FILE:x> or $<TARGET_BUNDLE_DIR:x>. At the
# end of the configure, cmake/tiers.cmake adds the command's target, if it names one (add_test runs that target's
# file), and fails the configure for a tier_pr test whose command is a path inside the build tree but no target's
# (nothing would build it for the pull request tier), or that was registered before this file was included.
# A test that needs a target its command line does not name (a program it starts by a path of its own) says so with the
# test property NEXUS_NEEDS (a list of targets).
#
# Include this before the first add_test.

include_guard(GLOBAL)

function(add_test)
    # Register it exactly as given: each argument passed as one bracket argument, so none is split or rewritten.
    set(call "_add_test(")
    math(EXPR last "${ARGC} - 1")
    if(ARGC GREATER 0)
        foreach(i RANGE ${last})
            string(APPEND call " [==[${ARGV${i}}]==]")
        endforeach()
    endif()
    string(APPEND call ")")
    cmake_language(EVAL CODE "${call}")

    if(ARGV0 STREQUAL "NAME")
        set(name "${ARGV1}")
        set(command "")
        set(in_command FALSE)
        set(seen_command FALSE)
        set(args "")
        if(ARGC GREATER 2)
            foreach(i RANGE 2 ${last})
                set(arg "${ARGV${i}}")
                if(arg STREQUAL "COMMAND")
                    set(in_command TRUE)
                    continue()
                elseif(arg MATCHES "^(CONFIGURATIONS|WORKING_DIRECTORY|COMMAND_EXPAND_LISTS)$")
                    set(in_command FALSE)
                    continue()
                endif()
                if(in_command)
                    if(NOT seen_command)
                        set(command "${arg}")
                        set(seen_command TRUE)
                    else()
                        list(APPEND args "${arg}")
                    endif()
                endif()
            endforeach()
        endif()
    else()
        set(name "${ARGV0}")
        set(command "${ARGV1}")
        set(args "")
        if(ARGC GREATER 2)
            foreach(i RANGE 2 ${last})
                list(APPEND args "${ARGV${i}}")
            endforeach()
        endif()
    endif()

    set(needs "")
    foreach(arg IN ITEMS "${command}" ${args})
        string(REGEX MATCHALL "\\$<TARGET_(FILE|FILE_DIR|LINKER_FILE|BUNDLE_DIR):[A-Za-z0-9_.+:-]+>" refs "${arg}")
        foreach(ref IN LISTS refs)
            string(REGEX REPLACE "^\\$<TARGET_[A-Z_]+:([^>]+)>$" "\\1" target "${ref}")
            list(APPEND needs "${target}")
        endforeach()
    endforeach()
    set_property(GLOBAL PROPERTY "NEXUS_TEST_RECORDED_${name}" TRUE)
    set_property(GLOBAL PROPERTY "NEXUS_TEST_NEEDS_${name}" "${needs}")
    # The command itself is resolved at the end of the configure (cmake/tiers.cmake), when every target is defined.
    set_property(GLOBAL PROPERTY "NEXUS_TEST_COMMAND_${name}" "${command}")
endfunction()
