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
