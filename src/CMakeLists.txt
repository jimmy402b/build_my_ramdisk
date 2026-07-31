# Source directory — targets added in later tasks.

add_library(myrd_common INTERFACE)
target_include_directories(myrd_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/common")
