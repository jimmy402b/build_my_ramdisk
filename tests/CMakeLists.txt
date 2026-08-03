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
