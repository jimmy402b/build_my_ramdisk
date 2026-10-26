# Asserts that a client target's *build graph* contains none of the execution libraries.
#
# T3-01 asks for a negative test proving the GUI binary carries no persistence, AIM or driver
# symbols.  With static libraries "no symbols" is not directly observable from a link line -- an
# unused archive contributes nothing -- so what is checked is the stronger, earlier fact: the
# target cannot even name them in its closure.  A link line that mentions an execution library is
# how such a dependency starts, and it is visible here before it is visible in a binary.
#
# **This runs at configure time and must be include()d, not run with `cmake -P`.**  A `-P` script
# has no targets, so it sees no closure and passes vacuously -- which is how the first version of
# this check reported two clean targets while checking nothing.
#
#   include(tools/check_client_link_closure.cmake)
#   myrd_assert_client_closure(myrd_cli)

set(MYRD_CLIENT_FORBIDDEN_LIBRARIES
    myrd_management myrd_persistence myrd_storage myrd_proxy myrd_driver
    myrd_service myrd_m5_worker_protocol myrd_m4_run myrd_m3_windows
    CACHE INTERNAL "Libraries a client target may never link")

function(myrd_assert_client_closure target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "myrd_assert_client_closure: no such target ${target}")
    endif()

    set(closure "${target}")
    set(changed TRUE)
    while(changed)
        set(changed FALSE)
        foreach(entry IN LISTS closure)
            if(NOT TARGET "${entry}")
                continue()
            endif()
            foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
                get_target_property(value "${entry}" "${property}")
                if(NOT value)
                    continue()
                endif()
                foreach(item IN LISTS value)
                    # A generator expression describes a link line this check cannot know until
                    # generate time; refusing it is better than passing on a closure that is not
                    # the one that will be linked.
                    if(item MATCHES "^\\$<")
                        message(FATAL_ERROR
                            "${target}'s closure contains a generator expression (${item}); the "
                            "check cannot see through it")
                    endif()
                    if(NOT item IN_LIST closure)
                        list(APPEND closure "${item}")
                        set(changed TRUE)
                    endif()
                endforeach()
            endforeach()
        endforeach()
    endwhile()

    set(found "")
    foreach(forbidden IN LISTS MYRD_CLIENT_FORBIDDEN_LIBRARIES)
        if(forbidden IN_LIST closure)
            list(APPEND found "${forbidden}")
        endif()
    endforeach()
    if(found)
        string(REPLACE ";" ", " found_text "${found}")
        message(FATAL_ERROR
            "${target} links execution libraries (${found_text}).  A client target may link only "
            "the contracts, the client library and the IPC framing -- see T3-01.")
    endif()

    # The reading, not just the verdict: the closure is written beside the build so a reviewer
    # can see what the boundary actually is.
    list(SORT closure)
    string(REPLACE ";" "\n" closure_text "${closure}")
    file(WRITE "${CMAKE_BINARY_DIR}/client-link-closure-${target}.txt" "${closure_text}\n")
    message(STATUS "${target}: client link closure is clean")
endfunction()
