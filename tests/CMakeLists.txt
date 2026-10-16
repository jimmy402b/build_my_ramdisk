add_library(myrd_test_support STATIC
    support/test_main.cpp
)
target_include_directories(myrd_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")

add_executable(myrd_common_tests
    unit/common_tests.cpp
)
target_link_libraries(myrd_common_tests PRIVATE myrd_test_support myrd_common)
target_include_directories(myrd_common_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_common_tests)

add_executable(myrd_protocol_tests
    unit/protocol_tests.cpp
)
target_link_libraries(myrd_protocol_tests PRIVATE myrd_test_support)
target_include_directories(myrd_protocol_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_protocol_tests)

add_executable(myrd_m1_operation_context_tests
    unit/operation_context_tests.cpp
)
target_link_libraries(myrd_m1_operation_context_tests PRIVATE myrd_test_support myrd_common)
target_include_directories(myrd_m1_operation_context_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_operation_context_tests)

include(CTest)
add_test(NAME unit.common COMMAND myrd_common_tests)
add_test(NAME unit.protocol COMMAND myrd_protocol_tests)
add_test(NAME unit.m1.operation_context COMMAND myrd_m1_operation_context_tests)

add_executable(myrd_m1_block_device_contract_tests
    unit/block_device_contract_tests.cpp
)
target_link_libraries(myrd_m1_block_device_contract_tests PRIVATE myrd_test_support myrd_storage)
target_include_directories(myrd_m1_block_device_contract_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_block_device_contract_tests)
add_test(NAME unit.m1.block_device_contract COMMAND myrd_m1_block_device_contract_tests)

add_executable(myrd_m1_protocol_tests
    unit/m1_protocol_tests.cpp
)
target_link_libraries(myrd_m1_protocol_tests PRIVATE myrd_test_support)
target_include_directories(myrd_m1_protocol_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_protocol_tests)
add_test(NAME unit.m1.protocol COMMAND myrd_m1_protocol_tests)

add_executable(myrd_m1_proxy_error_tests
    unit/proxy_error_tests.cpp
)
target_link_libraries(myrd_m1_proxy_error_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_m1_proxy_error_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_proxy_error_tests)
add_test(NAME unit.m1.proxy_error COMMAND myrd_m1_proxy_error_tests)

add_executable(myrd_m1_range_normalizer_tests
    unit/range_normalizer_tests.cpp
)
target_link_libraries(myrd_m1_range_normalizer_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_m1_range_normalizer_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_range_normalizer_tests)
add_test(NAME unit.m1.range_normalizer COMMAND myrd_m1_range_normalizer_tests)

add_executable(myrd_m1_request_processor_tests
    unit/m1_request_processor_tests.cpp
)
target_link_libraries(myrd_m1_request_processor_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_m1_request_processor_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_request_processor_tests)
add_test(NAME unit.m1.request_processor COMMAND myrd_m1_request_processor_tests)

add_executable(myrd_m1_protocol_fuzz_tests
    unit/m1_protocol_fuzz_tests.cpp
)
target_link_libraries(myrd_m1_protocol_fuzz_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_m1_protocol_fuzz_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_protocol_fuzz_tests)
add_test(NAME unit.m1.protocol_fuzz COMMAND myrd_m1_protocol_fuzz_tests)

add_executable(myrd_m1_namespace_policy_tests
    unit/namespace_policy_tests.cpp
)
target_link_libraries(myrd_m1_namespace_policy_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_m1_namespace_policy_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_namespace_policy_tests)
add_test(NAME unit.m1.namespace_policy COMMAND myrd_m1_namespace_policy_tests)

add_executable(myrd_m1_raw_protocol_io_tests
    unit/m1_raw_protocol_io_tests.cpp
)
target_link_libraries(myrd_m1_raw_protocol_io_tests PRIVATE myrd_test_support myrd_m1)
target_include_directories(myrd_m1_raw_protocol_io_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_raw_protocol_io_tests)
add_test(NAME unit.m1.raw_protocol_io COMMAND myrd_m1_raw_protocol_io_tests)

add_executable(myrd_m1_shm_session_tests
    integration/m1_shm_session_tests.cpp
)
target_link_libraries(myrd_m1_shm_session_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_m1_shm_session_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_shm_session_tests)
add_test(NAME integration.m1.shm COMMAND myrd_m1_shm_session_tests)
set_tests_properties(integration.m1.shm PROPERTIES TIMEOUT 20)

add_executable(myrd_m1_provider_tests
    unit/m1_provider_tests.cpp
)
target_link_libraries(myrd_m1_provider_tests PRIVATE myrd_test_support myrd_m1)
target_include_directories(myrd_m1_provider_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_provider_tests)
add_test(NAME unit.m1.provider COMMAND myrd_m1_provider_tests)
set_tests_properties(unit.m1.provider PROPERTIES TIMEOUT 20)

add_executable(myrd_m1_cli_tests unit/m1_cli_tests.cpp)
target_link_libraries(myrd_m1_cli_tests PRIVATE myrd_test_support myrd_m1)
target_include_directories(myrd_m1_cli_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_cli_tests)
add_test(NAME unit.m1.cli COMMAND myrd_m1_cli_tests)

# ---- M4 single/multi-disk CLI parser contract (T01) ----------------------
add_executable(myrd_m4_cli_tests
    unit/m4_cli_tests.cpp
)
target_link_libraries(myrd_m4_cli_tests PRIVATE myrd_test_support myrd_m4_run)
target_include_directories(myrd_m4_cli_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_cli_tests)
add_test(NAME unit.m4.cli COMMAND myrd_m4_cli_tests)

# ---- M5 durable profile-state codec (Task 5) --------------------------------
add_executable(myrd_m5_registration_codec_tests
    unit/m5_registration_codec_tests.cpp
)
target_link_libraries(myrd_m5_registration_codec_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_registration_codec_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_registration_codec_tests)
add_test(NAME unit.m5.registration_codec
         COMMAND myrd_m5_registration_codec_tests)

# ---- M5 execution backend against a real worker (Task 5) --------------------
# The suite that executes the launcher, the session provider and the adapter.
# It starts a real `myrd-m5-worker` process, so the binary is passed in as a
# compile definition and made a build dependency: a test that guessed the path
# would find a stale worker or none, and both look like product defects.
add_executable(myrd_m5_production_execution_backend_tests
    integration/m5_production_execution_backend_tests.cpp
)
# `myrd_m4_run` is linked for one case: the write-freeze re-entrancy case drives
# the *real* `Win32VolumeAdapter::lock` against the volume this worker actually
# mounted.  What a second lock attempt on a volume this process already holds does
# is a property of the production lock implementation and of Win32, not of any
# seam, so a fake would answer a different question.  No product library changes:
# this is a test executable reaching the real adapter.
target_link_libraries(myrd_m5_production_execution_backend_tests PRIVATE
    myrd_test_support myrd_management myrd_m4_run)
target_include_directories(myrd_m5_production_execution_backend_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
# A *relative* path with forward slashes, resolved at run time against this test
# module's own directory.  An absolute path cannot be pasted into a C++ literal:
# its backslashes would be read as escape sequences, and `\U` in `C:\Users\...`
# is not one of them.  Resolving relative to the module also keeps the build root
# out of the binary.
target_compile_definitions(myrd_m5_production_execution_backend_tests PRIVATE
    "MYRD_M5_WORKER_RELPATH=\"../../src/$<CONFIG>/myrd-m5-worker.exe\"")
add_dependencies(myrd_m5_production_execution_backend_tests myrd-m5-worker)
myrd_enable_warnings(myrd_m5_production_execution_backend_tests)
add_test(NAME m5.production_execution_backend
         COMMAND myrd_m5_production_execution_backend_tests)
set_tests_properties(m5.production_execution_backend PROPERTIES TIMEOUT 300)

# ---- M5 IWorkerControl adapter (Task 5) -------------------------------------
# Drives the adapter's fail-closed set and its observation mapping.  The paths
# that need a real worker -- the action mapping, the checkpoint boundary, the
# removal proof -- are covered by m5_production_execution_backend_tests against
# an actual process; `ServiceWorkerSession` is concrete over an authenticated
# channel and cannot be faked, so this suite deliberately does not reach for
# them.
add_executable(myrd_m5_service_worker_control_tests
    unit/m5_service_worker_control_tests.cpp
)
target_link_libraries(myrd_m5_service_worker_control_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_service_worker_control_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_service_worker_control_tests)
add_test(NAME unit.m5.service_worker_control
         COMMAND myrd_m5_service_worker_control_tests)

# ---- M4 exact volume ownership seams (T04; RED until implementation) -----
add_executable(myrd_m4_windows_volume_tests
    unit/m4_windows_volume_tests.cpp
)
target_link_libraries(myrd_m4_windows_volume_tests PRIVATE
    myrd_test_support myrd_m4_run)
target_include_directories(myrd_m4_windows_volume_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_windows_volume_tests)
add_test(NAME unit.m4.windows_volume COMMAND myrd_m4_windows_volume_tests)

add_executable(myrd_m4_disk_runtime_tests
    unit/m4_disk_runtime_tests.cpp
)
target_link_libraries(myrd_m4_disk_runtime_tests PRIVATE
    myrd_test_support myrd_m4_run)
target_include_directories(myrd_m4_disk_runtime_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_disk_runtime_tests)
add_test(NAME unit.m4.disk_runtime COMMAND myrd_m4_disk_runtime_tests)
set_tests_properties(unit.m4.disk_runtime PROPERTIES TIMEOUT 30)

# The shared production adapter (M5 Task 4 Step 1).  Both the foreground CLI
# and the per-profile worker compose through this one factory.
add_executable(myrd_m4_runtime_adapter_tests
    unit/m4_runtime_adapter_tests.cpp
)
target_link_libraries(myrd_m4_runtime_adapter_tests PRIVATE
    myrd_test_support myrd_m4_run)
target_include_directories(myrd_m4_runtime_adapter_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_runtime_adapter_tests)
add_test(NAME unit.m4.runtime_adapter COMMAND myrd_m4_runtime_adapter_tests)
set_tests_properties(unit.m4.runtime_adapter PROPERTIES TIMEOUT 30)

# The M5 private worker protocol (Task 5 Step 1): handshake authentication,
# replay rejection, binding mismatch, candidate-digest mismatch and the
# fail-closed structural decoder.
add_executable(myrd_m5_worker_protocol_tests
    unit/m5_worker_protocol_tests.cpp
)
target_link_libraries(myrd_m5_worker_protocol_tests PRIVATE
    myrd_test_support myrd_m5_worker_protocol)
target_include_directories(myrd_m5_worker_protocol_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_worker_protocol_tests)
add_test(NAME unit.m5.worker_protocol COMMAND myrd_m5_worker_protocol_tests)
set_tests_properties(unit.m5.worker_protocol PROPERTIES TIMEOUT 30)

# The worker's command loop (Task 5 Step 3): authentication before execution,
# the two-phase candidate gate, recovery-proof plumbing and worker-owned cleanup.
add_executable(myrd_m5_worker_session_tests
    unit/m5_worker_session_tests.cpp
)
target_link_libraries(myrd_m5_worker_session_tests PRIVATE
    myrd_test_support myrd_m5_worker_protocol)
target_include_directories(myrd_m5_worker_session_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_worker_session_tests)
add_test(NAME unit.m5.worker_session COMMAND myrd_m5_worker_session_tests)
set_tests_properties(unit.m5.worker_session PROPERTIES TIMEOUT 30)

# ---- M4 checkpoint and shutdown policy (T10; RED until implementation) ----
add_executable(myrd_m4_run_policy_tests
    unit/m4_run_policy_tests.cpp
)
target_link_libraries(myrd_m4_run_policy_tests PRIVATE
    myrd_test_support myrd_m4_run)
target_include_directories(myrd_m4_run_policy_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_run_policy_tests)
add_test(NAME unit.m4.run_policy COMMAND myrd_m4_run_policy_tests)

add_executable(myrd_m4_run_loop_runtime_tests
    integration/m4_run_loop_runtime_tests.cpp)
target_link_libraries(myrd_m4_run_loop_runtime_tests PRIVATE
    myrd_test_support myrd_m4_run)
target_include_directories(myrd_m4_run_loop_runtime_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_run_loop_runtime_tests)
add_test(NAME integration.m4.run_loop_runtime
    COMMAND myrd_m4_run_loop_runtime_tests)
set_tests_properties(integration.m4.run_loop_runtime PROPERTIES TIMEOUT 30)

add_executable(myrd_m4_persistence_adapter_tests
    unit/m4_persistence_adapter_tests.cpp)
target_link_libraries(myrd_m4_persistence_adapter_tests PRIVATE
    myrd_test_support myrd_m4_run myrd_m3_management)
target_include_directories(myrd_m4_persistence_adapter_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_persistence_adapter_tests)
add_test(NAME unit.m4.persistence_adapter
    COMMAND myrd_m4_persistence_adapter_tests)
set_tests_properties(unit.m4.persistence_adapter PROPERTIES TIMEOUT 60)

add_test(NAME unit.m4.provision_script
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/unit/test-m4-provision-script.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}")
set_tests_properties(unit.m4.provision_script PROPERTIES TIMEOUT 30)

# ---- M4 foreground composition (T13/T14) ---------------------------------
add_executable(myrd_m4_product_composition_tests
    integration/m4_product_composition_tests.cpp)
target_link_libraries(myrd_m4_product_composition_tests PRIVATE
    myrd_test_support)
target_include_directories(myrd_m4_product_composition_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
target_compile_definitions(myrd_m4_product_composition_tests PRIVATE
    MYRD_PRODUCT_EXE_PATH=L"$<TARGET_FILE:myrd>")
add_dependencies(myrd_m4_product_composition_tests myrd)
myrd_enable_warnings(myrd_m4_product_composition_tests)
add_test(NAME integration.m4.product_composition
    COMMAND myrd_m4_product_composition_tests)
set_tests_properties(integration.m4.product_composition PROPERTIES TIMEOUT 30)

add_executable(myrd_m4_multi_instance_tests
    integration/m4_multi_instance_tests.cpp)
target_link_libraries(myrd_m4_multi_instance_tests PRIVATE
    myrd_test_support myrd_m4_run)
target_include_directories(myrd_m4_multi_instance_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_multi_instance_tests)
add_test(NAME integration.m4.multi_instance
    COMMAND myrd_m4_multi_instance_tests)
set_tests_properties(integration.m4.multi_instance PROPERTIES
    LABELS "m4-v1" TIMEOUT 30)

add_executable(myrd_m1_lifecycle_runner_tests
    unit/m1_lifecycle_runner_tests.cpp
)
target_link_libraries(myrd_m1_lifecycle_runner_tests PRIVATE
    myrd_test_support myrd_m1 myrd_driver)
target_include_directories(myrd_m1_lifecycle_runner_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_lifecycle_runner_tests)
add_test(NAME unit.m1.lifecycle_runner COMMAND myrd_m1_lifecycle_runner_tests)

add_executable(myrd_m1_report_tests unit/m1_report_tests.cpp)
target_link_libraries(myrd_m1_report_tests PRIVATE
    myrd_test_support myrd_m1_report myrd_m1 myrd_poc)
target_include_directories(myrd_m1_report_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m1_report_tests)
add_test(NAME unit.m1.report COMMAND myrd_m1_report_tests)

add_test(NAME unit.m1.verification_script
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/unit/m1_verification_script_tests.ps1")
set_tests_properties(unit.m1.verification_script PROPERTIES TIMEOUT 30)

add_executable(myrd_storage_tests
    unit/storage_tests.cpp
)
target_link_libraries(myrd_storage_tests PRIVATE myrd_test_support myrd_storage)
target_include_directories(myrd_storage_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_storage_tests)
add_test(NAME unit.storage COMMAND myrd_storage_tests)

add_executable(myrd_request_processor_tests
    unit/request_processor_tests.cpp
)
target_link_libraries(myrd_request_processor_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_request_processor_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_request_processor_tests)
add_test(NAME unit.request_processor COMMAND myrd_request_processor_tests)

add_executable(myrd_object_names_tests
    unit/object_names_tests.cpp
)
target_link_libraries(myrd_object_names_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_object_names_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_object_names_tests)
add_test(NAME unit.object_names COMMAND myrd_object_names_tests)

add_executable(myrd_shm_proxy_server_tests
    integration/shm_proxy_server_tests.cpp
)
target_link_libraries(myrd_shm_proxy_server_tests PRIVATE myrd_test_support myrd_proxy)
target_include_directories(myrd_shm_proxy_server_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_shm_proxy_server_tests)
add_test(NAME integration.shm COMMAND myrd_shm_proxy_server_tests)

add_executable(myrd_srb_codec_tests
    unit/srb_codec_tests.cpp
)
target_link_libraries(myrd_srb_codec_tests PRIVATE myrd_test_support myrd_driver)
target_include_directories(myrd_srb_codec_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_srb_codec_tests)
add_test(NAME unit.srb_codec COMMAND myrd_srb_codec_tests)

add_executable(myrd_direct_aim_adapter_tests
    unit/direct_aim_adapter_tests.cpp
)
target_link_libraries(myrd_direct_aim_adapter_tests PRIVATE myrd_test_support myrd_driver)
target_include_directories(myrd_direct_aim_adapter_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_direct_aim_adapter_tests)
add_test(NAME unit.direct_aim_adapter COMMAND myrd_direct_aim_adapter_tests)

add_executable(myrd_physical_drive_locator_tests
    unit/physical_drive_locator_tests.cpp
)
target_link_libraries(myrd_physical_drive_locator_tests PRIVATE myrd_test_support myrd_driver)
target_include_directories(myrd_physical_drive_locator_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_physical_drive_locator_tests)
add_test(NAME unit.physical_drive_locator COMMAND myrd_physical_drive_locator_tests)

add_executable(myrd_report_tests
    unit/report_tests.cpp
)
target_link_libraries(myrd_report_tests PRIVATE myrd_test_support myrd_poc)
target_include_directories(myrd_report_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_report_tests)
add_test(NAME unit.report COMMAND myrd_report_tests)

add_executable(myrd_lifecycle_runner_tests
    unit/lifecycle_runner_tests.cpp
)
target_link_libraries(myrd_lifecycle_runner_tests PRIVATE myrd_test_support myrd_poc myrd_driver myrd_proxy)
target_include_directories(myrd_lifecycle_runner_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_lifecycle_runner_tests)
add_test(NAME unit.lifecycle_runner COMMAND myrd_lifecycle_runner_tests)

# Keep this snapshot after every accepted M1/legacy add_test call. It makes
# M2-V4 inherit the complete local regression baseline without a hand-maintained
# name list that can silently omit a newly registered M1 test.
get_property(_myrd_m2_v4_m1_regression_tests DIRECTORY PROPERTY TESTS)
if(NOT _myrd_m2_v4_m1_regression_tests)
    message(FATAL_ERROR "M2-V4 requires a non-empty accepted local test baseline")
endif()
set_property(TEST ${_myrd_m2_v4_m1_regression_tests}
    APPEND PROPERTY LABELS m2-v4)
set_property(TEST
    unit.m1.operation_context
    unit.m1.block_device_contract
    unit.storage
    APPEND PROPERTY LABELS m2-v1 m2-v2 m2-v3)

# Keep the M2 storage boundary executable as a continuous build-system gate.
# This test builds only the existing storage target (and its declared common
# dependency), so adding a proxy/driver dependency cannot hide behind the full
# product build.
foreach(_myrd_storage_link_property IN ITEMS
        LINK_LIBRARIES
        INTERFACE_LINK_LIBRARIES
        INTERFACE_LINK_LIBRARIES_DIRECT
        INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE
        MANUALLY_ADDED_DEPENDENCIES)
    get_target_property(_myrd_storage_link_items myrd_storage ${_myrd_storage_link_property})
    if(_myrd_storage_link_items)
        foreach(_myrd_storage_link_item IN LISTS _myrd_storage_link_items)
            foreach(_myrd_forbidden_storage_dependency IN ITEMS myrd_proxy myrd_driver)
                string(FIND "${_myrd_storage_link_item}"
                    "${_myrd_forbidden_storage_dependency}"
                    _myrd_forbidden_storage_dependency_index)
                if(NOT _myrd_forbidden_storage_dependency_index EQUAL -1)
                    message(FATAL_ERROR
                        "myrd_storage ${_myrd_storage_link_property} contains forbidden dependency ${_myrd_forbidden_storage_dependency}: ${_myrd_storage_link_item}")
                endif()
            endforeach()
        endforeach()
    endif()
endforeach()

set(_myrd_storage_boundary_script
    "${CMAKE_CURRENT_BINARY_DIR}/verify_storage_boundary_$<CONFIG>.ps1")
file(GENERATE OUTPUT "${_myrd_storage_boundary_script}" CONTENT [=[
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$ProbeBaseDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$Generator,

    [Parameter(Mandatory = $true)]
    [string]$Compiler
)

$ErrorActionPreference = 'Stop'

$sourceRoot = [System.IO.Path]::GetFullPath($SourceDirectory)
$probeBaseRoot = [System.IO.Path]::GetFullPath($ProbeBaseDirectory)
$expectedProbeBaseRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'out\build\storage-boundary-probes'))

if (-not $probeBaseRoot.Equals(
        $expectedProbeBaseRoot,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use unexpected storage-boundary probe base: $probeBaseRoot"
}

if (-not (Test-Path -LiteralPath $probeBaseRoot)) {
    New-Item -ItemType Directory -Path $probeBaseRoot -Force | Out-Null
}

$probeBaseItem = Get-Item -LiteralPath $probeBaseRoot -Force
if (($probeBaseItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Refusing to use reparse-point storage-boundary probe base: $probeBaseRoot"
}

$probeRunName = 'run-{0}-{1}' -f $PID, ([System.Guid]::NewGuid().ToString('N'))
$probeRoot = [System.IO.Path]::GetFullPath((Join-Path $probeBaseRoot $probeRunName))
$probePrefix = $probeBaseRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $probeRoot.StartsWith(
        $probePrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Generated storage-boundary probe path escaped its base: $probeRoot"
}

New-Item -ItemType Directory -Path $probeRoot | Out-Null

try {
    . (Join-Path $sourceRoot 'tools\Enter-MsvcEnvironment.ps1')

$savedErrorPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$configureArguments = @(
    '-S', $sourceRoot,
    '-B', $probeRoot,
    '-G', $Generator,
    '-DBUILD_TESTING:BOOL=OFF',
    "-DCMAKE_CXX_COMPILER:FILEPATH=$Compiler"
)
$configureOutput = @(& cmake.exe @configureArguments 2>&1)
$configureExitCode = $LASTEXITCODE
$ErrorActionPreference = $savedErrorPreference
$configureOutput | ForEach-Object { Write-Output $_ }

if ($configureExitCode -ne 0) {
    throw "Storage-boundary probe configure failed with exit code $configureExitCode"
}

$ErrorActionPreference = 'Continue'
$buildArguments = @(
    '--build', $probeRoot,
    '--config', $Configuration,
    '--target', 'myrd_storage',
    '--verbose'
)
$buildOutput = @(& cmake.exe @buildArguments 2>&1)
$buildExitCode = $LASTEXITCODE
$ErrorActionPreference = $savedErrorPreference
$buildOutput | ForEach-Object { Write-Output $_ }

if ($buildExitCode -ne 0) {
    throw "Storage-boundary probe build failed with exit code $buildExitCode"
}

$renderedBuildOutput = ($buildOutput | Out-String)
$forbiddenBuildPattern = '(?i)(CMakeFiles[\\/]myrd_(proxy|driver)\.dir|myrd_(proxy|driver)\.(lib|dll|exe))'
if ($renderedBuildOutput -match $forbiddenBuildPattern) {
    throw 'myrd_storage build traversed a forbidden proxy/driver target'
}

$forbiddenArtifacts = @(Get-ChildItem -LiteralPath $probeRoot -Recurse -File |
    Where-Object { $_.Name -match '(?i)^myrd_(proxy|driver)(\.|$)' })
if ($forbiddenArtifacts.Count -ne 0) {
    $paths = ($forbiddenArtifacts.FullName -join '; ')
    throw "myrd_storage produced forbidden proxy/driver artifacts: $paths"
}

$storageArtifacts = @(Get-ChildItem -LiteralPath $probeRoot -Recurse -File |
    Where-Object { $_.Name -ieq 'myrd_storage.lib' })
if ($storageArtifacts.Count -ne 1) {
    throw "Expected exactly one myrd_storage.lib, found $($storageArtifacts.Count)"
}

    Write-Output 'Storage boundary probe PASS: isolated build produced only the storage dependency closure.'
}
finally {
    if (Test-Path -LiteralPath $probeRoot) {
        $probeItem = Get-Item -LiteralPath $probeRoot -Force
        if (($probeItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to recursively remove reparse-point probe run: $probeRoot"
        }
        Remove-Item -LiteralPath $probeRoot -Recurse -Force
    }
}
]=])

add_test(
    NAME structural.m2.storage_independent_build
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${_myrd_storage_boundary_script}"
            -SourceDirectory "${CMAKE_SOURCE_DIR}"
            -ProbeBaseDirectory "${CMAKE_SOURCE_DIR}/out/build/storage-boundary-probes"
            -Configuration "$<CONFIG>"
            -Generator "${CMAKE_GENERATOR}"
            -Compiler "${CMAKE_CXX_COMPILER}")
set_tests_properties(structural.m2.storage_independent_build PROPERTIES
    LABELS "m2-v1;m2-v2;m2-v3;m2-v4"
    RUN_SERIAL TRUE)

# ---- M5 durable profile storage (Task 1) ----------------------------------
# The MYRDPRF binary profile format and its atomic store are transferred
# contracts: the codec round-trips byte-exact and malformed or unsupported
# input is refused without mutating the stored profile.
add_executable(myrd_m4_profile_codec_tests
    unit/m4_profile_codec_tests.cpp
)
target_link_libraries(myrd_m4_profile_codec_tests PRIVATE
    myrd_test_support myrd_profile)
target_include_directories(myrd_m4_profile_codec_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_profile_codec_tests)
add_test(NAME unit.m4.profile_codec COMMAND myrd_m4_profile_codec_tests)
set_tests_properties(unit.m4.profile_codec PROPERTIES TIMEOUT 60)

add_executable(myrd_m4_profile_store_tests
    unit/m4_profile_store_tests.cpp
)
target_link_libraries(myrd_m4_profile_store_tests PRIVATE
    myrd_test_support myrd_profile)
target_include_directories(myrd_m4_profile_store_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_profile_store_tests)
add_test(NAME unit.m4.profile_store COMMAND myrd_m4_profile_store_tests)
set_tests_properties(unit.m4.profile_store PROPERTIES TIMEOUT 120)

# ---- M5 durable operation and registration authority (Task 2) --------------
add_executable(myrd_m5_service_operation_store_tests
    integration/m5_service_operation_store_tests.cpp
)
target_link_libraries(myrd_m5_service_operation_store_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_service_operation_store_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_service_operation_store_tests)
add_test(NAME m5.service_operation_store
    COMMAND myrd_m5_service_operation_store_tests)
set_tests_properties(m5.service_operation_store PROPERTIES TIMEOUT 120)

# m5_service_orchestration_tests.cpp includes m4_checkpoint_operation_tests.cpp,
# so that model is compiled once here and once as its own target below.
add_executable(myrd_m5_service_orchestration_tests
    model/m5_service_orchestration_tests.cpp
)
target_link_libraries(myrd_m5_service_orchestration_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_service_orchestration_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/model")
myrd_enable_warnings(myrd_m5_service_orchestration_tests)
add_test(NAME m5.service_orchestration
    COMMAND myrd_m5_service_orchestration_tests)
set_tests_properties(m5.service_orchestration PROPERTIES TIMEOUT 120)

# ---- M5 execution scheduler (Task 7 Step 1) ---------------------------------
# The one executor of admitted work.  It includes the checkpoint model's fixture
# for its stores and its worker double, exactly as the orchestration suite does,
# and asserts the four claims Task 7 Step 1 names that this layer owns: durable
# admission precedes execution, only a claim this core handed out may be
# discharged, the scheduler is the serialization the command path holds, and a
# client's timeout or withdrawal never cancels the operation.
add_executable(myrd_m5_execution_scheduler_tests
    model/m5_execution_scheduler_tests.cpp
)
target_link_libraries(myrd_m5_execution_scheduler_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_execution_scheduler_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/model")
myrd_enable_warnings(myrd_m5_execution_scheduler_tests)
add_test(NAME m5.execution_scheduler
    COMMAND myrd_m5_execution_scheduler_tests)
set_tests_properties(m5.execution_scheduler PROPERTIES TIMEOUT 120)

# ---- M5 service lifecycle (T1-02) -------------------------------------------
# The service's own lifecycle with no SCM in it.  It includes the checkpoint
# model's fixture for its stores and its worker double, exactly as the
# orchestration suite does, and measures what the VM cannot measure cheaply:
# start and stop order, idempotence, the budget a stop hands down, and that an
# executor is never admitted over a store that could not be reconciled.
#
# What the VM adds for T1-02 is the SCM itself -- that a control manager can
# start the service, stop it, and reach it before the process is killed.
add_executable(myrd_m5_service_host_tests
    model/m5_service_host_tests.cpp
)
target_link_libraries(myrd_m5_service_host_tests PRIVATE
    myrd_test_support myrd_management myrd_service)
target_include_directories(myrd_m5_service_host_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/model")
myrd_enable_warnings(myrd_m5_service_host_tests)
add_test(NAME m5.service_host
    COMMAND myrd_m5_service_host_tests)
set_tests_properties(m5.service_host PROPERTIES TIMEOUT 120)

# ---- M5 worker sweep policy (T1-03) -----------------------------------------
# The startup sweep's *decisions*, with no job object and no processes in them:
# which orphans are ours, which may be terminated, and which leftovers may be
# reclaimed.  Every dangerous failure of a sweep is a decision failure -- cleaning
# up another profile's volume, terminating a process that inherited a dead
# worker's pid, authorising a cleanup from a half-recorded identity -- so the
# decisions are measured directly rather than only through a job enumeration.
add_executable(myrd_m5_worker_sweep_policy_tests
    model/m5_worker_sweep_policy_tests.cpp
)
target_link_libraries(myrd_m5_worker_sweep_policy_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_worker_sweep_policy_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_worker_sweep_policy_tests)
add_test(NAME m5.worker_sweep_policy
    COMMAND myrd_m5_worker_sweep_policy_tests)
set_tests_properties(m5.worker_sweep_policy PROPERTIES TIMEOUT 120)

# ---- M5 worker sweep orchestration (T1-03) ----------------------------------
# The order, the escalation and the report, over fakes.  A real orphan is not
# reproducible on demand, so the part that must not regress -- that an unreadable
# binding store stops the sweep instead of redirecting it into a mass termination,
# and that a refusal to ask is not silently escalated into a kill -- is measured
# here; the live thing is measured on the guest.
add_executable(myrd_m5_worker_sweep_tests
    model/m5_worker_sweep_tests.cpp
)
target_link_libraries(myrd_m5_worker_sweep_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_worker_sweep_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_worker_sweep_tests)
add_test(NAME m5.worker_sweep
    COMMAND myrd_m5_worker_sweep_tests)
set_tests_properties(m5.worker_sweep PROPERTIES TIMEOUT 120)

# ---- M5 worker fingerprint store (T1-03) ------------------------------------
# The encoding a sweep reads immediately after a crash to decide whether the
# process holding a device is its own.  Tested over an in-memory directory so it
# runs where the ACL-protected ProgramData root cannot be created; the real store
# is exercised on the guest.
add_executable(myrd_m5_worker_binding_store_tests
    model/m5_worker_binding_store_tests.cpp
)
target_link_libraries(myrd_m5_worker_binding_store_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_worker_binding_store_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_worker_binding_store_tests)
add_test(NAME m5.worker_binding_store
    COMMAND myrd_m5_worker_binding_store_tests)
set_tests_properties(m5.worker_binding_store PROPERTIES TIMEOUT 120)

# ---- M5 startup sweep against a real orphan (T1-03) -------------------------
# Creates real processes in the real named job, so it is VM-only by the
# development-machine rule (`AGENTS.md` section 4): it spawns processes and
# terminates them.  The decisions are measured over fakes elsewhere; what needs a
# real process is that a real orphan is actually ended, and that one which cannot
# be attributed is actually left alone.
add_executable(myrd_m5_startup_sweep_tests
    integration/m5_startup_sweep_tests.cpp
)
target_link_libraries(myrd_m5_startup_sweep_tests PRIVATE
    myrd_test_support myrd_management myrd_service myrd_m4_run)
target_include_directories(myrd_m5_startup_sweep_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
# The same relpath and worker dependency the backend suite carries, because this
# executable includes that suite's fixtures and therefore launches the same worker.
target_compile_definitions(myrd_m5_startup_sweep_tests PRIVATE
    "MYRD_M5_WORKER_RELPATH=\"../../src/$<CONFIG>/myrd-m5-worker.exe\"")
add_dependencies(myrd_m5_startup_sweep_tests myrd-m5-worker)
myrd_enable_warnings(myrd_m5_startup_sweep_tests)
add_test(NAME m5.startup_sweep
    COMMAND myrd_m5_startup_sweep_tests)
set_tests_properties(m5.startup_sweep PROPERTIES TIMEOUT 600)

# The recovery command family -- repair, rebind and forget -- named by Task 6's
# file list and missing until now.  It includes the checkpoint model's fixture
# for its fakes alone, exactly as the orchestration suite does.
add_executable(myrd_m4_recovery_commands_tests
    model/m4_recovery_commands_tests.cpp
)
target_link_libraries(myrd_m4_recovery_commands_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m4_recovery_commands_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/model")
myrd_enable_warnings(myrd_m4_recovery_commands_tests)
add_test(NAME m4.recovery_commands
    COMMAND myrd_m4_recovery_commands_tests)
set_tests_properties(m4.recovery_commands PROPERTIES TIMEOUT 120)

add_executable(myrd_m4_checkpoint_operation_tests
    model/m4_checkpoint_operation_tests.cpp
)
target_link_libraries(myrd_m4_checkpoint_operation_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m4_checkpoint_operation_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m4_checkpoint_operation_tests)
add_test(NAME m4.checkpoint_operation
    COMMAND myrd_m4_checkpoint_operation_tests)
set_tests_properties(m4.checkpoint_operation PROPERTIES TIMEOUT 120)

# ---- M5 execution boundary (Task 0) ---------------------------------------
# The gate is only worth having if it rejects a real violation, so this test
# configures throwaway projects against the real cmake/ExecutionBoundary.cmake
# and asserts both the rejections and the accepted contract-only target.
add_test(
    NAME structural.m5.execution_boundary
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/unit/test-m5-execution-boundary.ps1"
            -SourceDirectory "${CMAKE_SOURCE_DIR}"
            -ProbeBaseDirectory "${CMAKE_SOURCE_DIR}/out/build/execution-boundary-probes")
set_tests_properties(structural.m5.execution_boundary PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 120)

add_executable(myrd_m2_block_math_tests
    unit/block_math_tests.cpp
)
target_link_libraries(myrd_m2_block_math_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_block_math_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_block_math_tests)
add_test(NAME unit.m2.block_math COMMAND myrd_m2_block_math_tests)
set_tests_properties(unit.m2.block_math PROPERTIES LABELS "m2-v1;m2.1-v0")

add_executable(myrd_m2_consistent_snapshot_tests
    unit/dynamic_block_store_consistent_snapshot_tests.cpp
)
target_link_libraries(myrd_m2_consistent_snapshot_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_consistent_snapshot_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_consistent_snapshot_tests)
add_test(NAME unit.m3.m2_consistent_snapshot
    COMMAND myrd_m2_consistent_snapshot_tests)
set_tests_properties(unit.m3.m2_consistent_snapshot PROPERTIES
    LABELS "m2.1-v0;m2.1-v1;m2.1-v2;m2.1-v3;m2.1-v5;m3-v3"
    TIMEOUT 30)

add_executable(myrd_m2_consistent_snapshot_model_tests
    unit/dynamic_block_store_consistent_snapshot_model_tests.cpp
    support/consistent_snapshot_model.cpp
    support/consistent_snapshot_model.h
)
target_link_libraries(myrd_m2_consistent_snapshot_model_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_consistent_snapshot_model_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_consistent_snapshot_model_tests)
add_test(NAME unit.m3.m2_consistent_snapshot_model
    COMMAND myrd_m2_consistent_snapshot_model_tests)
set_tests_properties(unit.m3.m2_consistent_snapshot_model PROPERTIES
    LABELS "m2.1-v4;m3-v3"
    TIMEOUT 180)

add_executable(myrd_m2_provider_tests
    unit/m2_provider_tests.cpp
)
target_link_libraries(myrd_m2_provider_tests PRIVATE
    myrd_test_support myrd_m2)
target_include_directories(myrd_m2_provider_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_provider_tests)
add_test(NAME unit.m2.provider COMMAND myrd_m2_provider_tests)
set_tests_properties(unit.m2.provider PROPERTIES LABELS "m2-v4" TIMEOUT 30)

add_executable(myrd_m2_performance_probe_tests
    unit/m2_performance_probe_tests.cpp
)
target_link_libraries(myrd_m2_performance_probe_tests PRIVATE
    myrd_test_support myrd_m2)
target_include_directories(myrd_m2_performance_probe_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_performance_probe_tests)
add_test(NAME unit.m2.performance COMMAND myrd_m2_performance_probe_tests)
set_tests_properties(unit.m2.performance PROPERTIES
    LABELS "m2-v4"
    TIMEOUT 30)

add_executable(myrd_m2_shm_proxy_tests
    integration/m2_shm_proxy_tests.cpp
)
target_link_libraries(myrd_m2_shm_proxy_tests PRIVATE
    myrd_test_support myrd_m2)
target_include_directories(myrd_m2_shm_proxy_tests PRIVATE "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_shm_proxy_tests)
add_test(NAME integration.m2.shm COMMAND myrd_m2_shm_proxy_tests)
set_tests_properties(integration.m2.shm PROPERTIES LABELS "m2-v4" TIMEOUT 60)

add_executable(myrd_m2_virtual_memory_integration_tests
    integration/m2_virtual_memory_tests.cpp
)
target_link_libraries(myrd_m2_virtual_memory_integration_tests PRIVATE
    myrd_test_support myrd_storage Psapi)
target_include_directories(myrd_m2_virtual_memory_integration_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_virtual_memory_integration_tests)
add_test(NAME integration.m2.virtual_memory
    COMMAND myrd_m2_virtual_memory_integration_tests
    CONFIGURATIONS Release)
set_tests_properties(integration.m2.virtual_memory PROPERTIES
    LABELS "m2-v2"
    RUN_SERIAL TRUE
    TIMEOUT 180)

add_executable(myrd_m2_backing_store_tests
    unit/backing_store_tests.cpp
    support/fake_backing_store.h
)
target_link_libraries(myrd_m2_backing_store_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_backing_store_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_backing_store_tests)
add_test(NAME unit.m2.backing_store COMMAND myrd_m2_backing_store_tests)
set_tests_properties(unit.m2.backing_store PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 15)

add_executable(myrd_m2_virtual_memory_tests
    unit/virtual_memory_arena_tests.cpp
    support/fake_virtual_memory_api.h
)
target_link_libraries(myrd_m2_virtual_memory_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_virtual_memory_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_virtual_memory_tests)
add_test(NAME unit.m2.virtual_memory COMMAND myrd_m2_virtual_memory_tests)
set_tests_properties(unit.m2.virtual_memory PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 15)

add_executable(myrd_m2_block_directory_tests
    unit/block_directory_tests.cpp
)
target_link_libraries(myrd_m2_block_directory_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_block_directory_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_block_directory_tests)
add_test(NAME unit.m2.block_directory COMMAND myrd_m2_block_directory_tests)
set_tests_properties(unit.m2.block_directory PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 20)

add_executable(myrd_m2_store_core_tests
    unit/dynamic_block_store_core_tests.cpp
)
target_link_libraries(myrd_m2_store_core_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_store_core_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_store_core_tests)
add_test(NAME unit.m2.store_core COMMAND myrd_m2_store_core_tests)
set_tests_properties(unit.m2.store_core PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 20)

add_executable(myrd_m2_read_tests
    unit/dynamic_block_store_read_tests.cpp
)
target_link_libraries(myrd_m2_read_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_read_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_read_tests)
add_test(NAME unit.m2.read COMMAND myrd_m2_read_tests)
set_tests_properties(unit.m2.read PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 20)

add_executable(myrd_m2_limit_tests
    unit/dynamic_block_store_limit_tests.cpp
    support/fake_backing_store.h
    support/fake_virtual_memory_api.h
)
target_link_libraries(myrd_m2_limit_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_limit_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_limit_tests)
add_test(NAME unit.m2.limits COMMAND myrd_m2_limit_tests)
set_tests_properties(unit.m2.limits PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 20)

add_executable(myrd_m2_write_tests
    unit/dynamic_block_store_write_tests.cpp
    support/fake_backing_store.h
    support/fake_virtual_memory_api.h
)
target_link_libraries(myrd_m2_write_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_write_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_write_tests)
add_test(NAME unit.m2.write COMMAND myrd_m2_write_tests)
set_tests_properties(unit.m2.write PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 20)

add_executable(myrd_m2_range_tests
    unit/dynamic_block_store_range_tests.cpp
    support/fake_backing_store.h
)
target_link_libraries(myrd_m2_range_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_range_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_range_tests)
add_test(NAME unit.m2.ranges COMMAND myrd_m2_range_tests)
set_tests_properties(unit.m2.ranges PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 20)

add_executable(myrd_m2_snapshot_tests
    unit/dynamic_block_store_snapshot_tests.cpp
    support/fake_backing_store.h
)
target_link_libraries(myrd_m2_snapshot_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_snapshot_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_snapshot_tests)
add_test(NAME unit.m2.snapshot COMMAND myrd_m2_snapshot_tests)
set_tests_properties(unit.m2.snapshot PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 20)

add_executable(myrd_m2_backing_generation_tests
    unit/dynamic_block_store_backing_generation_tests.cpp
    support/fake_backing_store.h
)
target_link_libraries(myrd_m2_backing_generation_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_backing_generation_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_backing_generation_tests)
add_test(NAME unit.m2.backing_generation
    COMMAND myrd_m2_backing_generation_tests)
set_tests_properties(unit.m2.backing_generation PROPERTIES
    LABELS "m2-v1;m3-v3"
    TIMEOUT 20)

add_executable(myrd_m2_eviction_tests
    unit/dynamic_block_store_eviction_tests.cpp
)
target_link_libraries(myrd_m2_eviction_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_eviction_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_eviction_tests)
add_test(NAME unit.m2.eviction COMMAND myrd_m2_eviction_tests)
set_tests_properties(unit.m2.eviction PROPERTIES
    LABELS "m2-v1"
    TIMEOUT 20)

add_executable(myrd_m2_model_tests
    unit/dynamic_block_store_model_tests.cpp
    support/fake_backing_store.h
    support/reference_block_model.h
)
target_link_libraries(myrd_m2_model_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_model_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_model_tests)
add_test(NAME unit.m2.model COMMAND myrd_m2_model_tests)
set_tests_properties(unit.m2.model PROPERTIES
    LABELS "m2-v2;m2-a"
    TIMEOUT 180)

add_executable(myrd_m2_concurrency_tests
    unit/dynamic_block_store_concurrency_tests.cpp
    support/fake_backing_store.h
)
target_link_libraries(myrd_m2_concurrency_tests PRIVATE
    myrd_test_support myrd_storage)
target_include_directories(myrd_m2_concurrency_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_concurrency_tests)
add_test(NAME unit.m2.concurrency COMMAND myrd_m2_concurrency_tests)
set_tests_properties(unit.m2.concurrency PROPERTIES
    LABELS "m2-v3;m3-v3"
    TIMEOUT 30)

add_executable(myrd_m2_stress_tests
    unit/dynamic_block_store_stress_tests.cpp
    support/fake_backing_store.h
)
target_link_libraries(myrd_m2_stress_tests PRIVATE
    myrd_test_support myrd_storage Psapi)
target_include_directories(myrd_m2_stress_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_stress_tests)
add_test(NAME unit.m2.stress COMMAND myrd_m2_stress_tests)
set_tests_properties(unit.m2.stress PROPERTIES
    LABELS "m2-v3"
    TIMEOUT 120)

add_executable(myrd_m2_report_tests
    unit/m2_report_tests.cpp
)
target_link_libraries(myrd_m2_report_tests PRIVATE
    myrd_test_support myrd_m2_report)
target_include_directories(myrd_m2_report_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_report_tests)
add_test(NAME unit.m2.report COMMAND myrd_m2_report_tests)
set_tests_properties(unit.m2.report PROPERTIES
    LABELS "m2-v4;m2.1-v0"
    TIMEOUT 30)

add_executable(myrd_m2_lifecycle_runner_tests
    unit/m2_lifecycle_runner_tests.cpp
)
target_link_libraries(myrd_m2_lifecycle_runner_tests PRIVATE
    myrd_test_support myrd_m2_report)
target_include_directories(myrd_m2_lifecycle_runner_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_lifecycle_runner_tests)
add_test(NAME unit.m2.lifecycle COMMAND myrd_m2_lifecycle_runner_tests)
set_tests_properties(unit.m2.lifecycle PROPERTIES
    LABELS "m2-v4"
    TIMEOUT 30)

add_executable(myrd_m2_cli_tests
    unit/m2_cli_tests.cpp
)
target_link_libraries(myrd_m2_cli_tests PRIVATE
    myrd_test_support myrd_m2_report)
target_include_directories(myrd_m2_cli_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_cli_tests)
add_test(NAME unit.m2.cli COMMAND myrd_m2_cli_tests)
set_tests_properties(unit.m2.cli PROPERTIES
    LABELS "m2-v4"
    TIMEOUT 30)

add_executable(myrd_m2_local_fake_tests
    unit/m2_local_fake_tests.cpp
)
target_link_libraries(myrd_m2_local_fake_tests PRIVATE
    myrd_test_support myrd_m2_report)
target_include_directories(myrd_m2_local_fake_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_local_fake_tests)
add_test(NAME unit.m2.local_fake COMMAND myrd_m2_local_fake_tests)
set_tests_properties(unit.m2.local_fake PROPERTIES
    LABELS "m2-v4"
    TIMEOUT 30)

add_executable(myrd_m2_windows_acceptance_tests
    unit/m2_windows_acceptance_tests.cpp
)
target_link_libraries(myrd_m2_windows_acceptance_tests PRIVATE
    myrd_test_support myrd_m2_windows)
target_include_directories(myrd_m2_windows_acceptance_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m2_windows_acceptance_tests)
add_test(NAME unit.m2.windows_acceptance
    COMMAND myrd_m2_windows_acceptance_tests)
set_tests_properties(unit.m2.windows_acceptance PROPERTIES
    LABELS "m2-v5"
    TIMEOUT 30)

add_test(
    NAME integration.m2.cli_smoke
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/integration/m2_cli_smoke.ps1"
            -Executable "$<TARGET_FILE:myrd-m2-e2e>")
set_tests_properties(integration.m2.cli_smoke PROPERTIES
    LABELS "m2-v4"
    TIMEOUT 30)

add_test(
    NAME unit.m2.harness_preflight
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m2-harness-preflight.ps1")
set_tests_properties(unit.m2.harness_preflight PROPERTIES
    LABELS "m2-v5"
    TIMEOUT 30)

add_test(
    NAME unit.m2.native_process
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m2-native-process.ps1")
set_tests_properties(unit.m2.native_process PROPERTIES
    LABELS "m2-v5"
    TIMEOUT 30)

add_test(
    NAME unit.m2.json_verifier
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m2-json-verifier.ps1"
            -Executable "$<TARGET_FILE:myrd-m2-e2e>"
            -Verifier "${CMAKE_SOURCE_DIR}/tools/Verify-M2Json.ps1")
set_tests_properties(unit.m2.json_verifier PROPERTIES
    LABELS "m2-v5"
    TIMEOUT 30)

# ---- M3 checksum and codec tests (Task 10) ----------------------------------
add_executable(myrd_m3_checksum_codec_tests
    unit/m3_checksum_codec_tests.cpp
)
target_link_libraries(myrd_m3_checksum_codec_tests PRIVATE
    myrd_test_support myrd_persistence myrd_common)
target_include_directories(myrd_m3_checksum_codec_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_checksum_codec_tests)
add_test(NAME unit.m3.checksum_codec COMMAND myrd_m3_checksum_codec_tests)
set_tests_properties(unit.m3.checksum_codec PROPERTIES
    LABELS "m3-v0;m3-v1"
    TIMEOUT 20)

# ---- M3 exact file I/O tests (Task 11) -------------------------------------
add_executable(myrd_m3_file_io_tests
    unit/m3_file_io_tests.cpp
    support/fault_file.cpp
    support/fault_file.h
)
target_link_libraries(myrd_m3_file_io_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_file_io_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
myrd_enable_warnings(myrd_m3_file_io_tests)
add_test(NAME unit.m3.file_io COMMAND myrd_m3_file_io_tests)
set_tests_properties(unit.m3.file_io PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 30)

# ---- M3 secure bundle paths and identities (Task 12) -----------------------
add_executable(myrd_m3_writer_lock_helper
    helpers/m3_writer_lock_helper.cpp
)
target_link_libraries(myrd_m3_writer_lock_helper PRIVATE myrd_persistence)
target_include_directories(myrd_m3_writer_lock_helper PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_writer_lock_helper)

add_executable(myrd_m3_persistence_path_tests
    unit/m3_persistence_path_tests.cpp
)
add_dependencies(myrd_m3_persistence_path_tests myrd_m3_writer_lock_helper)
target_link_libraries(myrd_m3_persistence_path_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_persistence_path_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
myrd_enable_warnings(myrd_m3_persistence_path_tests)
add_test(NAME unit.m3.persistence_path COMMAND myrd_m3_persistence_path_tests)
set_tests_properties(unit.m3.persistence_path PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 30)

# ---- M3 persistence types tests (Task 13, T13.1) -------------------------------
add_executable(myrd_m3_persistence_types_tests
    unit/m3_persistence_types_tests.cpp
)
target_link_libraries(myrd_m3_persistence_types_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_persistence_types_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
target_compile_definitions(myrd_m3_persistence_types_tests PRIVATE
    "MYRD_M3_FAULT_PHASE_REGISTRY_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/fault-phase-registry.json\"")
if(MSVC)
    target_compile_options(myrd_m3_persistence_types_tests PRIVATE
        /we4061 /we4062)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(myrd_m3_persistence_types_tests PRIVATE
        -Werror=switch -Werror=switch-enum)
endif()
myrd_enable_warnings(myrd_m3_persistence_types_tests)
add_test(NAME unit.m3.persistence_types
    COMMAND myrd_m3_persistence_types_tests)
set_tests_properties(unit.m3.persistence_types PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 20)

# ---- M3 manifest slot codec tests (Task 14) ---------------------------------
add_executable(myrd_m3_manifest_format_tests
    unit/m3_manifest_format_tests.cpp
)
target_link_libraries(myrd_m3_manifest_format_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_manifest_format_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
target_compile_definitions(myrd_m3_manifest_format_tests PRIVATE
    "MYRD_M3_MANIFEST_V1_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/manifest-v1.hex\"")
myrd_enable_warnings(myrd_m3_manifest_format_tests)
add_test(NAME unit.m3.manifest_format
    COMMAND myrd_m3_manifest_format_tests)
set_tests_properties(unit.m3.manifest_format PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 30)

# ---- M3 complete manifest candidate selector (deferred Task 15) -----------
add_executable(myrd_m3_manifest_selector_tests
    unit/m3_manifest_selector_tests.cpp
    support/m3_reference_model.cpp
)
target_link_libraries(myrd_m3_manifest_selector_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_manifest_selector_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_manifest_selector_tests)
add_test(NAME unit.m3.manifest_selector
    COMMAND myrd_m3_manifest_selector_tests)
set_tests_properties(unit.m3.manifest_selector PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 60)

# ---- M3 journal v1 format tests (Task 16) ----------------------------------
add_executable(myrd_m3_journal_format_tests
    unit/m3_journal_format_tests.cpp
)
target_link_libraries(myrd_m3_journal_format_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_journal_format_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
target_compile_definitions(myrd_m3_journal_format_tests PRIVATE
    "MYRD_M3_JOURNAL_HEADER_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-header-v1.hex\""
    "MYRD_M3_JOURNAL_DATA_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-data-v1.hex\""
    "MYRD_M3_JOURNAL_ZERO_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-zero-v1.hex\""
    "MYRD_M3_JOURNAL_DISCARD_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-discard-v1.hex\""
    "MYRD_M3_JOURNAL_INDEX_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-index-v1.hex\""
    "MYRD_M3_JOURNAL_COMMIT_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-commit-v1.hex\""
    "MYRD_M3_JOURNAL_DIGEST_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-checkpoint-digest-v1.hex\"")
myrd_enable_warnings(myrd_m3_journal_format_tests)
add_test(NAME unit.m3.journal_format
    COMMAND myrd_m3_journal_format_tests)
set_tests_properties(unit.m3.journal_format PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 60)

# ---- M3 immutable journal index tests (Task 17) -----------------------------
add_executable(myrd_m3_journal_index_tests
    unit/m3_journal_index_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_journal_index_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_journal_index_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_journal_index_tests)
add_test(NAME unit.m3.journal_index
    COMMAND myrd_m3_journal_index_tests)
set_tests_properties(unit.m3.journal_index PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 60)

add_executable(myrd_m3_journal_index_builder_tests
    unit/m3_journal_index_builder_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_journal_index_builder_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_journal_index_builder_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_journal_index_builder_tests)
add_test(NAME unit.m3.journal_index_builder
    COMMAND myrd_m3_journal_index_builder_tests)
set_tests_properties(unit.m3.journal_index_builder PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 60)

add_executable(myrd_m3_journal_writer_tests
    unit/m3_journal_writer_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_journal_writer_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_journal_writer_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_journal_writer_tests)
add_test(NAME unit.m3.journal_writer
    COMMAND myrd_m3_journal_writer_tests)
set_tests_properties(unit.m3.journal_writer PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 60)

add_executable(myrd_m3_journal_reader_tests
    unit/m3_journal_reader_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_journal_reader_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_journal_reader_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_journal_reader_tests)
add_test(NAME unit.m3.journal_reader
    COMMAND myrd_m3_journal_reader_tests)
set_tests_properties(unit.m3.journal_reader PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 60)

# ---- M3 strict dynamic VHDX parser (Task 21) -------------------------------
add_executable(myrd_m3_vhdx_format_tests
    unit/m3_vhdx_format_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_vhdx_format_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_vhdx_format_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_vhdx_format_tests)
add_test(NAME unit.m3.vhdx_format
    COMMAND myrd_m3_vhdx_format_tests)
set_tests_properties(unit.m3.vhdx_format PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 60)

# ---- M3 lazy VHDX BAT/payload reader (Task 22) -----------------------------
add_executable(myrd_m3_vhdx_reader_tests
    unit/m3_vhdx_reader_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_vhdx_reader_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_vhdx_reader_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_vhdx_reader_tests)
add_test(NAME unit.m3.vhdx_reader
    COMMAND myrd_m3_vhdx_reader_tests)
set_tests_properties(unit.m3.vhdx_reader PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 90)

# ---- M3 sparse dynamic VHDX writer (Task 23) -------------------------------
add_executable(myrd_m3_vhdx_writer_tests
    unit/m3_vhdx_writer_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_vhdx_writer_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_vhdx_writer_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_vhdx_writer_tests)
add_test(NAME unit.m3.vhdx_writer
    COMMAND myrd_m3_vhdx_writer_tests)
set_tests_properties(unit.m3.vhdx_writer PROPERTIES
    LABELS "m3-v1"
    TIMEOUT 90)

# ---- M3 Windows native VHDX compatibility gate (Task 24) ------------------
add_executable(myrd_m3_windows_vhdx_compat_tests
    integration/m3_windows_vhdx_compat_tests.cpp
)
target_link_libraries(myrd_m3_windows_vhdx_compat_tests PRIVATE
    myrd_persistence myrd_driver)
target_include_directories(myrd_m3_windows_vhdx_compat_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_windows_vhdx_compat_tests)

add_test(
    NAME integration.m3.windows_vhdx
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-vhdx-compat.ps1"
            -Executable "$<TARGET_FILE:myrd_m3_windows_vhdx_compat_tests>")
set_tests_properties(integration.m3.windows_vhdx PROPERTIES
    LABELS "m3-v5"
    TIMEOUT 240)

# ---- M3 journal-first composite backing (Task 25) -------------------------
add_executable(myrd_m3_composite_backing_store_tests
    unit/m3_composite_backing_store_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_composite_backing_store_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_composite_backing_store_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_composite_backing_store_tests)
add_test(NAME unit.m3.composite_backing
    COMMAND myrd_m3_composite_backing_store_tests)
set_tests_properties(unit.m3.composite_backing PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 60)

# ---- M3 complete-generation recovery (Task 26) -----------------------------
add_executable(myrd_m3_recovery_tests
    integration/m3_recovery_tests.cpp
)
target_link_libraries(myrd_m3_recovery_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_recovery_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_recovery_tests)
add_test(NAME integration.m3.recovery COMMAND myrd_m3_recovery_tests)
set_tests_properties(integration.m3.recovery PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 90)

# ---- M3 independent complete-recovery oracle (Task 26) ---------------------
add_executable(myrd_m3_recovery_oracle_tests
    integration/m3_recovery_oracle_tests.cpp
    support/m3_independent_recovery_fixture.cpp
)
target_link_libraries(myrd_m3_recovery_oracle_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_recovery_oracle_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
target_compile_definitions(myrd_m3_recovery_oracle_tests PRIVATE
    "MYRD_M3_RECOVERY_HEADER_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-header-v1.hex\""
    "MYRD_M3_RECOVERY_DATA_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-data-v1.hex\""
    "MYRD_M3_RECOVERY_ZERO_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-zero-v1.hex\""
    "MYRD_M3_RECOVERY_DISCARD_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-discard-v1.hex\""
    "MYRD_M3_RECOVERY_INDEX_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-index-v1.hex\""
    "MYRD_M3_RECOVERY_COMMIT_FIXTURE_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/journal-commit-v1.hex\"")
myrd_enable_warnings(myrd_m3_recovery_oracle_tests)
add_test(NAME integration.m3.recovery_oracle
    COMMAND myrd_m3_recovery_oracle_tests)
set_tests_properties(integration.m3.recovery_oracle PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 90)

# ---- M3 generation-zero image creation (Task 27) --------------------------
add_executable(myrd_m3_image_create_tests
    integration/m3_image_create_tests.cpp
)
target_link_libraries(myrd_m3_image_create_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_image_create_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_image_create_tests)
add_test(NAME integration.m3.image_create
    COMMAND myrd_m3_image_create_tests)
set_tests_properties(integration.m3.image_create PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 120)

# ---- M3 recovered backing into M2 first-touch loading (Task 28) ------------
add_executable(myrd_m3_lazy_backing_tests
    integration/m3_lazy_backing_tests.cpp
)
target_link_libraries(myrd_m3_lazy_backing_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_lazy_backing_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_lazy_backing_tests)
add_test(NAME integration.m3.lazy_backing
    COMMAND myrd_m3_lazy_backing_tests)
set_tests_properties(integration.m3.lazy_backing PROPERTIES
    LABELS "m3-v3"
    TIMEOUT 120)

# ---- M3 persistence boundary test (Task 00) ----------------------------------
add_executable(myrd_persistence_boundary_tests
    unit/persistence_boundary_tests.cpp
)
target_link_libraries(myrd_persistence_boundary_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_persistence_boundary_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_persistence_boundary_tests)
add_test(NAME unit.m3.boundary COMMAND myrd_persistence_boundary_tests)
set_tests_properties(unit.m3.boundary PROPERTIES
    LABELS "m3-v0"
    TIMEOUT 20)

# ---- M3 image session state machine (Task 29) -------------------------------
add_executable(myrd_m3_image_session_tests
    unit/m3_image_session_tests.cpp
)
target_link_libraries(myrd_m3_image_session_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_image_session_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_image_session_tests)
add_test(NAME unit.m3.image_session COMMAND myrd_m3_image_session_tests)
set_tests_properties(unit.m3.image_session PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 30)

# ---- M3 pre-manifest checkpoint preparation (Task 30) ---------------------
add_executable(myrd_m3_checkpoint_tests
    integration/m3_checkpoint_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_checkpoint_tests PRIVATE
    myrd_test_support myrd_persistence myrd_storage)
target_include_directories(myrd_m3_checkpoint_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
myrd_enable_warnings(myrd_m3_checkpoint_tests)
add_test(NAME integration.m3.checkpoint COMMAND myrd_m3_checkpoint_tests)
set_tests_properties(integration.m3.checkpoint PROPERTIES
    LABELS "m3-v2;m3-v3"
    TIMEOUT 120)

# ---- M3 manifest publication and commit resolution (Task 31) -------------
add_executable(myrd_m3_commit_publish_tests
    integration/m3_commit_publish_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_commit_publish_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_commit_publish_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
target_compile_definitions(myrd_m3_commit_publish_tests PRIVATE
    "MYRD_M3_FAULT_PHASE_REGISTRY_PATH=\"${CMAKE_SOURCE_DIR}/tests/fixtures/m3/fault-phase-registry.json\"")
myrd_enable_warnings(myrd_m3_commit_publish_tests)
add_test(NAME integration.m3.commit_publish
    COMMAND myrd_m3_commit_publish_tests)
set_tests_properties(integration.m3.commit_publish PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 120)

add_executable(myrd_m3_commit_publish_phase_runner
    integration/m3_commit_publish_phase_runner.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_commit_publish_phase_runner PRIVATE
    myrd_persistence)
target_include_directories(myrd_m3_commit_publish_phase_runner PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_commit_publish_phase_runner)
file(READ
    "${CMAKE_SOURCE_DIR}/tests/fixtures/m3/fault-phase-registry.json"
    myrd_m3_fault_phase_registry_json)
string(JSON myrd_m3_fault_phase_count LENGTH
    "${myrd_m3_fault_phase_registry_json}" phases)
math(EXPR myrd_m3_fault_phase_last "${myrd_m3_fault_phase_count} - 1")
set(myrd_m3_manifest_fault_phases)
foreach(index RANGE 0 ${myrd_m3_fault_phase_last})
    string(JSON phase_id GET
        "${myrd_m3_fault_phase_registry_json}" phases ${index} id)
    if(phase_id MATCHES "^manifest\\.")
        list(APPEND myrd_m3_manifest_fault_phases "${phase_id}")
    endif()
endforeach()
list(LENGTH myrd_m3_manifest_fault_phases
    myrd_m3_manifest_fault_phase_count)
if(NOT myrd_m3_manifest_fault_phase_count EQUAL 10)
    message(FATAL_ERROR
        "Task31 requires exactly 10 manifest fault registry phases")
endif()
foreach(phase IN LISTS myrd_m3_manifest_fault_phases)
    add_test(
        NAME integration.m3.commit_publish.phase.${phase}
        COMMAND myrd_m3_commit_publish_phase_runner ${phase})
    set_tests_properties(
        integration.m3.commit_publish.phase.${phase}
        PROPERTIES LABELS "m3-v2" TIMEOUT 30)
endforeach()

# ---- M3 checkpoint runtime reconciliation (Task 32) -----------------------
add_executable(myrd_m3_checkpoint_reconcile_tests
    integration/m3_checkpoint_reconcile_tests.cpp
    support/fault_file.cpp
)
target_link_libraries(myrd_m3_checkpoint_reconcile_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_checkpoint_reconcile_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_checkpoint_reconcile_tests)
add_test(NAME integration.m3.checkpoint_reconcile
    COMMAND myrd_m3_checkpoint_reconcile_tests)
set_tests_properties(integration.m3.checkpoint_reconcile PROPERTIES
    LABELS "m3-v2;m3-v3"
    TIMEOUT 120)

# ---- M3 independent checkpoint crash model (Task 33) ----------------------
execute_process(
    COMMAND git log -1 --format=%H -- src
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE MYRD_M3_TASK33_SOURCE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT MYRD_M3_TASK33_SOURCE_COMMIT)
    set(MYRD_M3_TASK33_SOURCE_COMMIT "source-archive")
endif()
add_executable(myrd_m3_checkpoint_crash_model_tests
    model/m3_checkpoint_crash_model_tests.cpp
)
target_link_libraries(myrd_m3_checkpoint_crash_model_tests PRIVATE
    myrd_test_support
    myrd_persistence
    myrd_storage
)
target_include_directories(myrd_m3_checkpoint_crash_model_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(myrd_m3_checkpoint_crash_model_tests PRIVATE
    "MYRD_M3_CHECKPOINT_CRASH_MATRIX_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/m3/crash-matrix.json\""
    "MYRD_M3_FAULT_PHASE_REGISTRY_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/m3/fault-phase-registry.json\""
    "MYRD_M3_TASK33_SOURCE_COMMIT=\"${MYRD_M3_TASK33_SOURCE_COMMIT}\""
)
myrd_enable_warnings(myrd_m3_checkpoint_crash_model_tests)
add_test(NAME model.m3.checkpoint_crash
    COMMAND myrd_m3_checkpoint_crash_model_tests)
set_tests_properties(model.m3.checkpoint_crash PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 180)

# ---- M3 immutable live-extent enumeration (Task 34) -----------------------
add_executable(myrd_m3_live_extent_tests
    unit/m3_live_extent_tests.cpp
)
target_link_libraries(myrd_m3_live_extent_tests PRIVATE
    myrd_test_support myrd_persistence)
target_include_directories(myrd_m3_live_extent_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_live_extent_tests)
add_test(NAME unit.m3.live_extents COMMAND myrd_m3_live_extent_tests)
set_tests_properties(unit.m3.live_extents PROPERTIES
    LABELS "m3-v4"
    TIMEOUT 120)

# ---- M3 committed-image compaction (Task 35) -----------------------------
add_executable(myrd_m3_compaction_tests
    integration/m3_compaction_tests.cpp
)
target_link_libraries(myrd_m3_compaction_tests PRIVATE
    myrd_test_support myrd_persistence myrd_storage)
target_include_directories(myrd_m3_compaction_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_compaction_tests)
add_test(NAME integration.m3.compaction COMMAND myrd_m3_compaction_tests)
set_tests_properties(integration.m3.compaction PROPERTIES
    LABELS "m3-v4"
    TIMEOUT 180)

# ---- M3 safe immutable-artifact retention (Task 36) ---------------------
add_executable(myrd_m3_retention_tests
    integration/m3_retention_tests.cpp
)
target_link_libraries(myrd_m3_retention_tests PRIVATE
    myrd_test_support myrd_persistence myrd_storage)
target_include_directories(myrd_m3_retention_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_retention_tests)
add_test(NAME integration.m3.retention COMMAND myrd_m3_retention_tests)
set_tests_properties(integration.m3.retention PROPERTIES
    LABELS "m3-v4"
    TIMEOUT 120)

# ---- M3 deterministic management reports (Task 37) ----------------------
add_executable(myrd_m3_report_tests
    unit/m3_report_tests.cpp
)
target_link_libraries(myrd_m3_report_tests PRIVATE
    myrd_test_support myrd_m3_management)
target_include_directories(myrd_m3_report_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_report_tests)
add_test(NAME unit.m3.report COMMAND myrd_m3_report_tests)
set_tests_properties(unit.m3.report PROPERTIES
    LABELS "m3-v2;m3-v4"
    TIMEOUT 30)
add_executable(myrd_m3_report_fixture
    helpers/m3_report_fixture.cpp)
target_link_libraries(myrd_m3_report_fixture PRIVATE myrd_m3_management)
target_include_directories(myrd_m3_report_fixture PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_report_fixture)
add_test(NAME unit.m3.json_verifier
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/unit/m3_json_verifier_tests.ps1"
        -Verifier "${CMAKE_SOURCE_DIR}/tools/Verify-M3Json.ps1"
        -FixtureExecutable "$<TARGET_FILE:myrd_m3_report_fixture>")
set_tests_properties(unit.m3.json_verifier PROPERTIES
    LABELS "m3-v2;m3-v4"
    TIMEOUT 30)

# ---- M3 lifecycle CLI (Task 38) -------------------------------------------
add_executable(myrd_m3_cli_tests
    integration/m3_cli_tests.cpp
)
target_link_libraries(myrd_m3_cli_tests PRIVATE
    myrd_test_support myrd_m3_management)
target_include_directories(myrd_m3_cli_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_cli_tests)
add_test(NAME integration.m3.cli COMMAND myrd_m3_cli_tests)
set_tests_properties(integration.m3.cli PROPERTIES
    LABELS "m3-v2;m3-v4"
    TIMEOUT 120)

add_executable(myrd_m3_lifecycle_runner_tests
    integration/m3_lifecycle_runner_tests.cpp)
target_link_libraries(myrd_m3_lifecycle_runner_tests PRIVATE
    myrd_test_support myrd_m3_management)
target_include_directories(myrd_m3_lifecycle_runner_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_lifecycle_runner_tests)
add_test(NAME integration.m3.lifecycle_runner
    COMMAND myrd_m3_lifecycle_runner_tests)
set_tests_properties(integration.m3.lifecycle_runner PROPERTIES
    LABELS "m3-v2;m3-v4"
    TIMEOUT 120)

add_executable(myrd_m5_service_worker_session_tests
    unit/m5_service_worker_session_tests.cpp
)
target_link_libraries(myrd_m5_service_worker_session_tests PRIVATE
    myrd_test_support myrd_management)
target_include_directories(myrd_m5_service_worker_session_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_service_worker_session_tests)
add_test(NAME unit.m5.service_worker_session
    COMMAND myrd_m5_service_worker_session_tests)
set_tests_properties(unit.m5.service_worker_session PROPERTIES TIMEOUT 120)

add_executable(myrd_m5_worker_private_pipe_tests
    unit/m5_worker_private_pipe_tests.cpp
)
target_link_libraries(myrd_m5_worker_private_pipe_tests PRIVATE
    myrd_test_support myrd_m5_worker_protocol)
target_include_directories(myrd_m5_worker_private_pipe_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_worker_private_pipe_tests)
add_test(NAME unit.m5.worker_private_pipe
    COMMAND myrd_m5_worker_private_pipe_tests)
set_tests_properties(unit.m5.worker_private_pipe PROPERTIES TIMEOUT 120)

# ---- M5 Task 3 addendum: read-only checkpoint recovery inspection ----------
# The 2026-10-02 solution re-opens Task 3 for `inspect_checkpoint`, the only
# read-side source of a committed native operation id and therefore the only
# thing the Task 5 recovery proof may judge ownership against.
add_executable(myrd_m5_checkpoint_inspection_tests
    integration/m5_checkpoint_inspection_tests.cpp)
target_link_libraries(myrd_m5_checkpoint_inspection_tests PRIVATE
    myrd_test_support myrd_m3_management)
target_include_directories(myrd_m5_checkpoint_inspection_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m5_checkpoint_inspection_tests)
add_test(NAME integration.m5.checkpoint_inspection
    COMMAND myrd_m5_checkpoint_inspection_tests)
set_tests_properties(integration.m5.checkpoint_inspection PROPERTIES
    TIMEOUT 120)

add_executable(myrd_m3_provider_integration_tests
    integration/m3_provider_integration_tests.cpp)
target_link_libraries(myrd_m3_provider_integration_tests PRIVATE
    myrd_test_support myrd_m3_management)
target_include_directories(myrd_m3_provider_integration_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_provider_integration_tests)
add_test(NAME integration.m3.provider
    COMMAND myrd_m3_provider_integration_tests)
set_tests_properties(integration.m3.provider PROPERTIES
    LABELS "m3-v2;m3-v4"
    TIMEOUT 120)

add_executable(myrd_m3_crash_test_control_tests
    unit/m3_crash_test_control_tests.cpp)
target_link_libraries(myrd_m3_crash_test_control_tests PRIVATE
    myrd_test_support myrd_m3_management)
target_include_directories(myrd_m3_crash_test_control_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_crash_test_control_tests)
add_test(NAME unit.m3.crash_test_control
    COMMAND myrd_m3_crash_test_control_tests)
set_tests_properties(unit.m3.crash_test_control PROPERTIES
    LABELS "m3-v2;m3-v6"
    TIMEOUT 30)

add_executable(myrd_m3_windows_acceptance_tests
    integration/m3_windows_acceptance_tests.cpp)
target_link_libraries(myrd_m3_windows_acceptance_tests PRIVATE
    myrd_test_support myrd_m3_windows)
target_include_directories(myrd_m3_windows_acceptance_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_windows_acceptance_tests)
add_test(NAME integration.m3.windows_acceptance
    COMMAND myrd_m3_windows_acceptance_tests)
set_tests_properties(integration.m3.windows_acceptance PROPERTIES
    LABELS "m3-v2;m3-v5"
    TIMEOUT 120)

add_test(NAME integration.m3.aim_server_cli
    COMMAND myrd-m3-aim-server --help)
set_tests_properties(integration.m3.aim_server_cli PROPERTIES
    LABELS "m3-v5"
    TIMEOUT 30)

# ---- M3 safe real-host preflight harness (Task 40) ------------------------
add_test(
    NAME unit.m3.harness_preflight
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-harness-preflight.ps1")
set_tests_properties(unit.m3.harness_preflight PROPERTIES
    LABELS "m3-v5"
    TIMEOUT 60)

add_test(
    NAME unit.m3.ntfs_workload_preflight
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-ntfs-workload.ps1")
set_tests_properties(unit.m3.ntfs_workload_preflight PROPERTIES
    LABELS "m3-v5"
    TIMEOUT 60)

add_test(
    NAME unit.m3.ntfs_verifier
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-ntfs-verifier.ps1")
set_tests_properties(unit.m3.ntfs_verifier PROPERTIES
    LABELS "m3-v5"
    TIMEOUT 60)

add_test(
    NAME unit.m3.volume_device_path
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-volume-device-path.ps1")
set_tests_properties(unit.m3.volume_device_path PROPERTIES
    LABELS "m3-v6"
    TIMEOUT 60)

add_test(
    NAME integration.m3.cli_local_e2e
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-cli-local-e2e.ps1"
        -Configuration "$<CONFIG>")
set_tests_properties(integration.m3.cli_local_e2e PROPERTIES
    LABELS "m3-v5"
    TIMEOUT 60)

# ---- M3 simulated crash evidence route (Task 42, limited scope) ----------
add_test(
    NAME unit.m3.crash_verifier
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-crash-verifier.ps1")
set_tests_properties(unit.m3.crash_verifier PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 60)

add_test(
    NAME integration.m3.crash_simulated_evidence
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-crash-simulated.ps1"
        -Configuration "$<CONFIG>"
        -CrashModelExecutable "$<TARGET_FILE:myrd_m3_checkpoint_crash_model_tests>")
set_tests_properties(integration.m3.crash_simulated_evidence PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 240)

add_test(
    NAME integration.m3.crash_routing
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-crash-routing.ps1"
        -Configuration "$<CONFIG>"
        -CrashModelExecutable "$<TARGET_FILE:myrd_m3_checkpoint_crash_model_tests>")
set_tests_properties(integration.m3.crash_routing PROPERTIES
    LABELS "m3-v2"
    TIMEOUT 240)

add_test(
    NAME unit.m3.real_crash_contract
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-real-crash-contract.ps1")
set_tests_properties(unit.m3.real_crash_contract PROPERTIES
    LABELS "m3-v2;m3-v6"
    TIMEOUT 60)

add_test(
    NAME unit.m3.real_crash_aggregate
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-real-crash-aggregate.ps1")

# ---- M3 Task43 soak evidence verifier -------------------------------------
add_executable(myrd_m3_soak_tests
    integration/m3_soak_tests.cpp)
target_link_libraries(myrd_m3_soak_tests PRIVATE
    myrd_m3_management myrd_persistence myrd_storage myrd_common Psapi)
target_include_directories(myrd_m3_soak_tests PRIVATE
    "${CMAKE_SOURCE_DIR}")
myrd_enable_warnings(myrd_m3_soak_tests)
add_test(NAME integration.m3.soak_workload
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-soak-seed.ps1"
        -Executable "$<TARGET_FILE:myrd_m3_soak_tests>")
set_tests_properties(integration.m3.soak_workload PROPERTIES
    LABELS "m3-v7"
    TIMEOUT 120)

add_test(
    NAME integration.m3.soak_runner
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-soak-runner.ps1"
        -Runner "${CMAKE_CURRENT_SOURCE_DIR}/e2e/Invoke-M3Soak.ps1"
        -Executable "$<TARGET_FILE:myrd_m3_soak_tests>")
set_tests_properties(integration.m3.soak_runner PROPERTIES
    LABELS "m3-v7"
    TIMEOUT 240)

add_test(
    NAME integration.m3.soak_routing
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-soak-routing.ps1"
        -Route "${CMAKE_SOURCE_DIR}/tools/Invoke-M3Verification.ps1"
        -Executable "$<TARGET_FILE:myrd_m3_soak_tests>")
set_tests_properties(integration.m3.soak_routing PROPERTIES
    LABELS "m3-v7"
    TIMEOUT 240)

add_test(
    NAME unit.m3.soak_verifier
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m3-soak-verifier.ps1")
set_tests_properties(unit.m3.soak_verifier PROPERTIES
    LABELS "m3-v7"
    TIMEOUT 60)
set_tests_properties(unit.m3.real_crash_aggregate PROPERTIES
    LABELS "m3-v2;m3-v6"
    TIMEOUT 60)

add_test(
    NAME integration.m2.orchestrator_whatif
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m2-orchestrator-whatif.ps1"
            -Executable "$<TARGET_FILE:myrd-m2-e2e>"
            -Orchestrator "${CMAKE_SOURCE_DIR}/tools/Invoke-M2Verification.ps1")
set_tests_properties(integration.m2.orchestrator_whatif PROPERTIES
    LABELS "m2-v5"
    TIMEOUT 30)

add_test(
    NAME unit.m2.orchestrator_report_gate
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test-m2-orchestrator-report-gate.ps1"
            -Executable "$<TARGET_FILE:myrd-m2-e2e>"
            -Common "${CMAKE_SOURCE_DIR}/tools/M2Verification.Common.ps1"
            -Verifier "${CMAKE_SOURCE_DIR}/tools/Verify-M2Json.ps1")
set_tests_properties(unit.m2.orchestrator_report_gate PROPERTIES
    LABELS "m2-v5"
    TIMEOUT 30)
