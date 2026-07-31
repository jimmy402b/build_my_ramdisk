# Source directory — targets added in later tasks.

add_library(myrd_common INTERFACE)
target_include_directories(myrd_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/common")

add_library(myrd_storage STATIC
    storage/memory_block_device.cpp
    storage/memory_block_device.h
    storage/block_device.h
)
target_include_directories(myrd_storage PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_storage PUBLIC myrd_common)
myrd_enable_warnings(myrd_storage)
