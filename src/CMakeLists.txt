# Source directory — targets added in later tasks.

add_library(myrd_m4_run STATIC
    run/cli.cpp run/cli.h
    run/windows_volume.cpp run/windows_volume.h
    run/disk_runtime.cpp run/disk_runtime.h
    run/runtime_adapter.cpp run/runtime_adapter.h
    run/m4_persistence_adapter.cpp run/m4_persistence_adapter.h
    run/run_loop.cpp run/run_loop.h
    run/driver_preflight.cpp run/driver_preflight.h
    run/run_resources.rc)
target_include_directories(myrd_m4_run PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m4_run PUBLIC bcrypt)
target_link_libraries(myrd_m4_run PUBLIC myrd_m3_windows)
myrd_enable_warnings(myrd_m4_run)

add_executable(myrd
    run/main.cpp
    run/run_resources.rc)
target_include_directories(myrd PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd PRIVATE myrd_m4_run)
set_target_properties(myrd PROPERTIES OUTPUT_NAME "myrd")
if(BUILD_TESTING)
    # The executable-level composition tests use environment-controlled seams
    # to exercise preflight and create/recover dispatch without touching real
    # drivers or volumes.  These seams are absent from production artifacts.
    target_compile_definitions(myrd PRIVATE MYRD_ENABLE_TEST_SEAMS=1)
endif()
myrd_enable_warnings(myrd)

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
    persistence/live_extent_enumerator.h
    persistence/live_extent_enumerator.cpp
    persistence/recovery_engine.h
    persistence/recovery_engine.cpp
    persistence/image_inspector.h
    persistence/image_inspector.cpp
    persistence/production_policy.h
    persistence/production_policy.cpp
    persistence/image_creator.h
    persistence/image_creator.cpp
    persistence/recovered_store_adapter.h
    persistence/recovered_store_adapter.cpp
    persistence/image_session.h
    persistence/image_session.cpp
    persistence/checkpoint_coordinator.h
    persistence/checkpoint_coordinator.cpp
    persistence/compaction_coordinator.h
    persistence/compaction_coordinator.cpp
    persistence/retention_manager.h
    persistence/retention_manager.cpp
    persistence/checkpoint_reconciler.cpp
    persistence/commit_resolver.cpp
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

# Management/reporting remains above persistence and cannot introduce proxy,
# driver, M1 or M2 dependencies into the persistence core.
add_library(myrd_m3_management STATIC
    m3/m3_cli.h
    m3/m3_cli.cpp
    m3/m3_lifecycle_runner.h
    m3/m3_lifecycle_runner.cpp
    m3/m3_provider.h
    m3/m3_provider.cpp
    m3/m3_report.h
    m3/m3_report.cpp
    m3/m3_evidence_publisher.h
    m3/m3_evidence_publisher.cpp
)
if(BUILD_TESTING)
    target_sources(myrd_m3_management PRIVATE
        m3/m3_crash_test_control.h
        m3/m3_crash_test_control.cpp)
endif()
target_include_directories(myrd_m3_management PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m3_management PUBLIC
    myrd_persistence myrd_common myrd_proxy)
myrd_enable_warnings(myrd_m3_management)

# AIM-facing composition is an upper-layer adapter. It may depend on the
# accepted M1 lifecycle contract, but persistence itself remains driver-free.
add_library(myrd_m3_windows STATIC
    m3/m3_windows_acceptance.h
    m3/m3_windows_acceptance.cpp
)
target_include_directories(myrd_m3_windows PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m3_windows PUBLIC
    myrd_m3_management myrd_m1 myrd_driver)
myrd_enable_warnings(myrd_m3_windows)

add_executable(myrd_m3_e2e
    m3/main.cpp)
target_include_directories(myrd_m3_e2e PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_m3_e2e PRIVATE myrd_m3_management)
myrd_enable_warnings(myrd_m3_e2e)

add_executable(myrd-m3-aim-server
    m3/m3_aim_server_main.cpp)
target_include_directories(myrd-m3-aim-server PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd-m3-aim-server PRIVATE myrd_m3_windows)
myrd_enable_warnings(myrd-m3-aim-server)

# ---- M5 product contracts (Task 1) ------------------------------------------
# Uuid / error / state / quantity / json value types shared by the service, the
# CLI and the IPC layer. Contract-only: no execution, no driver, no persistence.
add_library(myrd_product_contracts STATIC
    product/json_writer.cpp product/json_writer.h
    product/product_error.cpp product/product_error.h
    product/product_state.cpp product/product_state.h
    product/product_version.h
    product/quantity_parser.cpp product/quantity_parser.h
    product/uuid.cpp product/uuid.h
)
target_include_directories(myrd_product_contracts PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_product_contracts PUBLIC myrd_common)
myrd_enable_warnings(myrd_product_contracts)

# Typed command, IPC framing, pipe transport and authorization. Reaches the
# runtime only over the pipe, so it must never link an execution target.
add_library(myrd_product_ipc STATIC
    control/authorization.cpp control/authorization.h
    control/command.cpp control/command.h
    control/ipc_codec.cpp control/ipc_codec.h
    control/ipc_frame.cpp control/ipc_frame.h
    control/pipe_transport.cpp control/pipe_transport.h
    control/public_pipe_client.cpp control/public_pipe_client.h
    control/public_pipe_server.cpp control/public_pipe_server.h
)
target_include_directories(myrd_product_ipc PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_product_ipc PUBLIC myrd_product_contracts myrd_common)
myrd_enable_warnings(myrd_product_ipc)
myrd_assert_execution_boundary(myrd_product_ipc)

# Binary MYRDPRF profile: codec, strict validation, atomic durable store.
add_library(myrd_profile STATIC
    profile/profile_codec.cpp profile/profile_codec.h
    profile/profile_store.cpp profile/profile_store.h
    profile/profile_types.cpp profile/profile_types.h
    profile/profile_validation.cpp profile/profile_validation.h
)
target_include_directories(myrd_profile PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_profile PUBLIC myrd_product_contracts myrd_common)
myrd_enable_warnings(myrd_profile)
myrd_assert_execution_boundary(myrd_profile)

# ---- M5 management authority (Task 2) ----------------------------------------
# Durable operation records, profile registration, the transition contract and
# the ServiceCore durable model. Execution-agnostic by construction: ServiceCore
# drives execution through IWorkerControl, whose sole implementation is the
# rebuilt worker control from Task 5. Nothing here may link an execution target.
add_library(myrd_management STATIC
    management/execution_scheduler.cpp management/execution_scheduler.h
    management/operation_store.cpp management/operation_store.h
    management/profile_registration_codec.cpp management/profile_registration_codec.h
    management/profile_registration_store.cpp management/profile_registration_store.h
    management/service_core.cpp management/service_core.h
    management/service_operation_store.cpp management/service_operation_store.h
    management/service_worker_control.cpp management/service_worker_control.h
    management/service_worker_session.cpp management/service_worker_session.h
    management/state_machine.cpp management/state_machine.h
    management/windows_worker_observation.h
    management/windows_worker_sessions.cpp management/windows_worker_sessions.h
    management/worker_sweep_policy.cpp management/worker_sweep_policy.h
)
target_include_directories(myrd_management PUBLIC "${CMAKE_SOURCE_DIR}")
# `myrd_m5_worker_protocol` is the private channel and its codec.  It is not a
# forbidden execution dependency: it reaches no runtime, no driver and no bundle
# -- the worker does that, behind the pipe.  Linking it is what lets the service
# speak to a worker at all without regaining the execution coupling the boundary
# check exists to prevent.
target_link_libraries(myrd_management PUBLIC
    myrd_profile myrd_product_ipc myrd_product_contracts
    myrd_m5_worker_protocol myrd_common)
myrd_enable_warnings(myrd_management)
myrd_assert_execution_boundary(myrd_management)

# ---- M5 service lifecycle (Task T1-02) ---------------------------------------
# The service's own lifecycle with no SCM in it: bring the durable state up,
# admit work, and come down within a budget.  The SCM protocol lives in the
# executable above this, and the execution lives behind the worker control below
# it, so this target is ordinary linkable code and its start/stop paths are
# measured by ordinary tests rather than only by installing a service.
add_library(myrd_service STATIC
    service/service_host.cpp service/service_host.h
    service/service_runtime.cpp service/service_runtime.h
    service/service_system.cpp service/service_system.h
)
target_include_directories(myrd_service PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd_service PUBLIC myrd_management myrd_common)
myrd_enable_warnings(myrd_service)
myrd_assert_execution_boundary(myrd_service)

# The service executable.  It is the only target that speaks to the SCM, and it
# holds no execution coupling: the runtime it drives is out of process, behind
# the per-profile worker, which is why the boundary check applies to it too.
add_executable(myrd-service
    service/service_main.cpp
)
target_include_directories(myrd-service PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd-service PRIVATE myrd_service Advapi32)
myrd_enable_warnings(myrd-service)
myrd_assert_execution_boundary(myrd-service)

# ---- M5 private worker protocol and session (Task 5) -------------------------
# The typed service <-> per-profile worker messages, their strict codec, the
# single definition of the M5 native checkpoint operation id rule
# (`src/common/m5_native_operation_id.h`), and the worker's command loop.
#
# The session drives execution through the `WorkerRuntime` interface and never
# names a concrete runtime, so this target stays free of execution, driver and
# management dependencies -- both the worker executable and the service-side
# adapter link it.
#
# It does include `src/persistence/image_inspector.h`, because the P4 recovery
# correlation it carries *is* the persistence layer's
# `CheckpointRecoveryCorrelation`: one definition rather than two field sets to
# keep in sync (see the transfer manifest, §7 item 15).  That is an include
# dependency only -- no persistence symbol is called from here, so this target
# does not link `myrd_persistence` and the management layer that links this
# target does not gain a persistence link through it.
add_library(myrd_m5_worker_protocol STATIC
    host/m5_worker_bytes.h
    host/m5_worker_protocol.cpp host/m5_worker_protocol.h
    host/m5_worker_session.cpp host/m5_worker_session.h
    host/m5_worker_private_pipe.cpp host/m5_worker_private_pipe.h
)
target_include_directories(myrd_m5_worker_protocol PUBLIC "${CMAKE_SOURCE_DIR}")
# Advapi32 is only for the private pipe's security descriptor
# (`ConvertStringSecurityDescriptorToSecurityDescriptorW`); it is deliberately
# not the public pipe's descriptor.
target_link_libraries(myrd_m5_worker_protocol PUBLIC
    myrd_product_contracts myrd_common Advapi32)
myrd_enable_warnings(myrd_m5_worker_protocol)
myrd_assert_execution_boundary(myrd_m5_worker_protocol)

# The per-profile worker.  This is the one target allowed to reach DiskRuntime
# and M3 (Task 0 Step 3), so it deliberately does *not* call
# myrd_assert_execution_boundary: `myrd_m4_run` and `myrd_m3_windows` are exactly
# what it is here to link.  Task 7 launches it, service-side.
add_executable(myrd-m5-worker
    host/m5_worker_main.cpp
    host/m5_worker_runtime.cpp host/m5_worker_runtime.h
    # The provisioning script, for the same reason `myrd` carries it: it is a
    # process resource, and `run_provisioning_helper` resolves it from
    # `GetModuleHandleW(nullptr)` -- the *executable*, not the static library the
    # volume code is linked from.  Without it the worker creates a generation-zero
    # image, starts AIM, and then dies in `provision_new_volume` with
    # ERROR_RESOURCE_TYPE_NOT_FOUND, which is what the create path did before
    # this line existed.
    run/run_resources.rc
)
target_include_directories(myrd-m5-worker PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(myrd-m5-worker PRIVATE
    myrd_m5_worker_protocol myrd_m4_run myrd_m3_management)
myrd_enable_warnings(myrd-m5-worker)
