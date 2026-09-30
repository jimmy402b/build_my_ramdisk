# M5 execution-boundary enforcement.
#
# The single execution authority in this product is:
#   public pipe -> ServiceCore -> per-profile worker -> DiskRuntime -> M3 -> phdskmnt
#
# Only the M4 foreground executable (`myrd`) and the M5 per-profile worker
# executable may link the execution-authority libraries. Management, CLI and GUI
# targets must reach the runtime exclusively through IPC, so that a second
# in-process path to bundle/volume/driver state can never appear.
#
# Call myrd_assert_execution_boundary(<target>) on every management, CLI or GUI
# target as it is created. A violation fails the configure step rather than
# waiting for a runtime surprise.
function(myrd_assert_execution_boundary target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "myrd_assert_execution_boundary called for unknown target ${target}")
    endif()
    foreach(property IN ITEMS
            LINK_LIBRARIES
            INTERFACE_LINK_LIBRARIES
            INTERFACE_LINK_LIBRARIES_DIRECT
            INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE
            MANUALLY_ADDED_DEPENDENCIES)
        get_target_property(items ${target} ${property})
        if(items)
            foreach(item IN LISTS items)
                foreach(forbidden IN ITEMS
                        myrd_m4_run
                        myrd_m3_windows
                        myrd_m3_management
                        myrd_m1
                        myrd_driver
                        myrd_proxy)
                    string(FIND "${item}" "${forbidden}" index)
                    if(NOT index EQUAL -1)
                        message(FATAL_ERROR
                            "${target} ${property} contains forbidden execution dependency "
                            "${forbidden}: ${item}. Management/CLI/GUI targets must reach the "
                            "runtime only through the public pipe "
                            "(see cmake/ExecutionBoundary.cmake).")
                    endif()
                endforeach()
            endforeach()
        endif()
    endforeach()
endfunction()
