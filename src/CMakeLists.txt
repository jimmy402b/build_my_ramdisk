# Source directory — targets added in later tasks.

add_library(myrd_common STATIC
    common/sha256.cpp common/sha256.h
    common/authenticode.cpp common/authenticode.h
    common/process_snapshot.cpp common/process_snapshot.h
)
target_include_directories(myrd_common PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_common PUBLIC bcrypt wintrust crypt32)
myrd_enable_warnings(myrd_common)

add_library(myrd_storage STATIC
    storage/memory_block_device.cpp
    storage/memory_block_device.h
    storage/block_device.h
)
target_include_directories(myrd_storage PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_storage PUBLIC myrd_common)
myrd_enable_warnings(myrd_storage)

add_library(myrd_proxy STATIC
    proxy/request_processor.cpp
    proxy/request_processor.h
)
target_include_directories(myrd_proxy PUBLIC "${CMAKE_SOURCE_DIR}")
target_sources(myrd_proxy PRIVATE
    proxy/object_names.cpp proxy/object_names.h
    proxy/shared_memory_channel.cpp proxy/shared_memory_channel.h
    proxy/shm_proxy_server.cpp proxy/shm_proxy_server.h
)
target_link_libraries(myrd_proxy PUBLIC myrd_storage bcrypt)
myrd_enable_warnings(myrd_proxy)

add_library(myrd_driver STATIC
    driver/srb_codec.cpp driver/srb_codec.h
    driver/srb_transport.h
    driver/direct_aim_adapter.cpp driver/direct_aim_adapter.h
    driver/adapter_discovery.cpp driver/adapter_discovery.h
    driver/windows_srb_transport.cpp driver/windows_srb_transport.h
    driver/physical_drive_locator.cpp driver/physical_drive_locator.h
)
target_include_directories(myrd_driver PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_driver PUBLIC myrd_common Setupapi Cfgmgr32)
myrd_enable_warnings(myrd_driver)
