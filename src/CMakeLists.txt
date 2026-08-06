# Source directory — targets added in later tasks.

add_library(myrd_common STATIC
    common/sha256.cpp common/sha256.h
    common/sha256_stream.cpp common/sha256_stream.h
    common/sha256_stream_state.h
    common/crc32c.cpp common/crc32c.h
    common/authenticode.cpp common/authenticode.h
    common/process_snapshot.cpp common/process_snapshot.h
)
target_include_directories(myrd_common PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_common PUBLIC bcrypt wintrust crypt32)
myrd_enable_warnings(myrd_common)

add_library(myrd_storage STATIC
    storage/memory_block_device.cpp
    storage/memory_block_device.h
    storage/backing_store.h
    storage/block_device.h
    storage/block_constants.h
    storage/block_math.h
    storage/dynamic_block_types.h
    storage/dynamic_block_store_internal.h
    storage/dynamic_block_store_core.cpp
    storage/dynamic_block_store_read.cpp
    storage/dynamic_block_store_write.cpp
    storage/dynamic_block_store_ranges.cpp
    storage/dynamic_block_store_snapshot.cpp
    storage/snapshot_coordinator.cpp
    storage/snapshot_coordinator.h
    storage/dynamic_block_store_backing.cpp
    storage/dynamic_block_store.h
    storage/eviction_manager.cpp
    storage/eviction_manager.h
    storage/block_directory.cpp
    storage/block_directory.h
    storage/virtual_memory_arena.cpp
    storage/virtual_memory_arena.h
    storage/zero_backing_store.cpp
    storage/zero_backing_store.h
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
    m1/m1_cli.cpp m1/m1_cli.h
    m1/m1_lifecycle_runner.cpp m1/m1_lifecycle_runner.h
)
target_include_directories(myrd_m1 PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m1 PUBLIC myrd_proxy myrd_storage myrd_common)
myrd_enable_warnings(myrd_m1)

add_library(myrd_m1_report STATIC
    m1/m1_report.cpp m1/m1_report.h
)
target_include_directories(myrd_m1_report PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m1_report PUBLIC myrd_m1 myrd_poc)
myrd_enable_warnings(myrd_m1_report)

add_library(myrd_m2 STATIC
    m2/m2_provider.cpp m2/m2_provider.h
    m2/m2_performance_probe.cpp m2/m2_performance_probe.h
    m2/m2_volume_workload.cpp m2/m2_volume_workload.h
)
target_include_directories(myrd_m2 PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m2 PUBLIC myrd_proxy myrd_storage myrd_common)
myrd_enable_warnings(myrd_m2)

add_library(myrd_m2_report STATIC
    m2/m2_report.cpp m2/m2_report.h
    m2/m2_lifecycle_runner.cpp m2/m2_lifecycle_runner.h
    m2/m2_cli.cpp m2/m2_cli.h
    m2/m2_local_fake.cpp m2/m2_local_fake.h
)
target_include_directories(myrd_m2_report PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m2_report PUBLIC myrd_m2 myrd_poc)
myrd_enable_warnings(myrd_m2_report)

add_library(myrd_m2_windows STATIC
    m2/m2_windows_acceptance.cpp m2/m2_windows_acceptance.h
)
target_include_directories(myrd_m2_windows PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m2_windows PUBLIC
    myrd_m2_report myrd_driver myrd_m1 Psapi Version Advapi32)
myrd_enable_warnings(myrd_m2_windows)

add_executable(myrd-m1-e2e
    m1/main.cpp
)
target_include_directories(myrd-m1-e2e PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd-m1-e2e PRIVATE myrd_m1_report myrd_m1 myrd_poc myrd_driver myrd_proxy myrd_storage myrd_common bcrypt wintrust crypt32 version Setupapi Cfgmgr32)
myrd_enable_warnings(myrd-m1-e2e)

add_executable(myrd-m2-e2e
    m2/main.cpp
)
target_link_libraries(myrd-m2-e2e PRIVATE myrd_m2_windows)
myrd_enable_warnings(myrd-m2-e2e)

add_executable(myrd-m0-poc
    poc/main.cpp
)
target_include_directories(myrd-m0-poc PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd-m0-poc PRIVATE myrd_poc myrd_driver myrd_proxy myrd_storage myrd_common bcrypt wintrust crypt32 Setupapi Cfgmgr32)
myrd_enable_warnings(myrd-m0-poc)

# ---- M3 persistence library (Task 00) ---------------------------------------
# Persistence core must not depend on driver, proxy, M1, M2 or virtdisk.
add_library(myrd_persistence STATIC
    persistence/persistence_types.h
    persistence/persistence_types.cpp
    persistence/manifest_format.h
    persistence/manifest_format.cpp
    persistence/manifest_selector.h
    persistence/manifest_selector.cpp
    persistence/journal_format.h
    persistence/journal_format.cpp
    persistence/journal_index.h
    persistence/journal_index.cpp
    persistence/journal_writer.h
    persistence/journal_writer.cpp
    persistence/journal_reader.h
    persistence/journal_reader.cpp
    persistence/vhdx_format.h
    persistence/vhdx_format.cpp
    persistence/vhdx_reader.h
    persistence/vhdx_reader.cpp
    persistence/vhdx_writer.h
    persistence/vhdx_writer.cpp
    persistence/composite_backing_store.h
    persistence/composite_backing_store.cpp
    persistence/recovery_engine.h
    persistence/recovery_engine.cpp
    persistence/file_io.h
    persistence/file_io.cpp
    persistence/file_identity.h
    persistence/file_identity.cpp
    persistence/native_rename_backend.h
    persistence/native_sparse_backend.h
    persistence/image_paths.h
    persistence/image_paths.cpp
    persistence/writer_lock.h
    persistence/writer_lock.cpp
)
target_include_directories(myrd_persistence PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_persistence PUBLIC myrd_storage myrd_common bcrypt)
if(BUILD_TESTING)
    # Test seams are absent from production (BUILD_TESTING=OFF) artifacts.
    target_compile_definitions(myrd_persistence PUBLIC MYRD_ENABLE_TEST_SEAMS=1)
endif()
myrd_enable_warnings(myrd_persistence)
