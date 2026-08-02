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
    proxy/proxy_error.cpp proxy/proxy_error.h
    proxy/range_normalizer.cpp proxy/range_normalizer.h
    proxy/imdproxy_protocol.h
)
target_link_libraries(myrd_proxy PUBLIC myrd_storage bcrypt)
target_sources(myrd_proxy PRIVATE
    proxy/shm_proxy_session.cpp proxy/shm_proxy_session.h
)
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

add_library(myrd_poc STATIC
    poc/json_writer.cpp poc/json_writer.h
    poc/poc_report.cpp poc/poc_report.h
    poc/report_publisher.cpp poc/report_publisher.h
    poc/cli.cpp poc/cli.h
    poc/lifecycle_runner.cpp poc/lifecycle_runner.h
    poc/raw_round_trip.cpp poc/raw_round_trip.h
    poc/m0_provider.cpp poc/m0_provider.h
    poc/windows_raw_disk_io.cpp poc/windows_raw_disk_io.h
)
target_include_directories(myrd_poc PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_poc PUBLIC myrd_common myrd_proxy myrd_storage)
myrd_enable_warnings(myrd_poc)

add_library(myrd_m1 STATIC
    m1/m1_provider.cpp m1/m1_provider.h
    m1/m1_raw_protocol_io.cpp m1/m1_raw_protocol_io.h
)
target_include_directories(myrd_m1 PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m1 PUBLIC myrd_proxy myrd_storage myrd_common)
myrd_enable_warnings(myrd_m1)

add_executable(myrd-m0-poc
    poc/main.cpp
)
target_include_directories(myrd-m0-poc PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd-m0-poc PRIVATE myrd_poc myrd_driver myrd_proxy myrd_storage myrd_common bcrypt wintrust crypt32 Setupapi Cfgmgr32)
myrd_enable_warnings(myrd-m0-poc)
