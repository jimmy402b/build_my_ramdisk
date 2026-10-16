cmake_minimum_required(VERSION 3.25)
project(MyRamDiskM0 VERSION 0.1.0 LANGUAGES CXX)

# **Have the compiler itself report header dependencies, so a header change rebuilds every consumer.**
#
# This closes the project's most expensive paper cut.  Without it, MSVC's dependency information is
# incomplete enough that changing a header leaves translation units built against the old one, and the
# result is not a compile error but a mismatch that surfaces later as a crash or as a test that fails
# only sometimes.  The project has been carrying the consequence as a rule rather than a mechanism --
# "after touching a header you must rebuild with --clean-first" -- and it has been bitten at least
# twice: section 8 item 4 records it as an unrooted ABI trap, and part 63 found it as the likely reason
# `m5.execution_scheduler` was intermittently red.
#
# A rule that depends on remembering is not a fix; this is.  With the flag on, MSVC emits `/showIncludes`
# and CMake records what each object actually included, so the build graph is right by construction and
# `--clean-first` stops being load-bearing.
#
# It must be set before the project is configured, hence its position here, and it is unconditional so a
# fresh configure picks it up without anyone needing to know.
set(CMAKE_DEPENDS_USE_COMPILER TRUE)

if(NOT MSVC)
    message(FATAL_ERROR "M0 requires MSVC")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "M0 requires x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(cmake/Warnings.cmake)
include(cmake/ExecutionBoundary.cmake)
add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
