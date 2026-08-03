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

# M2-V4 always includes the complete accepted local M1 regression baseline.
# The three shared storage-contract tests are real prerequisites reserved in
# every local M2 tier; later tasks append their own behavior tests per tier.
set(_myrd_m2_v4_m1_regression_tests
    unit.common
    unit.protocol
    unit.m1.operation_context
    unit.m1.block_device_contract
    unit.m1.protocol
    unit.m1.proxy_error
    unit.m1.range_normalizer
    unit.m1.request_processor
    unit.m1.protocol_fuzz
    unit.m1.namespace_policy
    unit.m1.raw_protocol_io
    integration.m1.shm
    unit.m1.provider
    unit.m1.cli
    unit.m1.lifecycle_runner
    unit.m1.report
    unit.m1.verification_script
    unit.storage
    unit.request_processor
    unit.object_names
    integration.shm
    unit.srb_codec
    unit.direct_aim_adapter
    unit.physical_drive_locator
    unit.report
    unit.lifecycle_runner
)
set_tests_properties(${_myrd_m2_v4_m1_regression_tests}
    PROPERTIES LABELS "m2-v4")
set_property(TEST
    unit.m1.operation_context
    unit.m1.block_device_contract
    unit.storage
    APPEND PROPERTY LABELS m2-v1 m2-v2 m2-v3)

# Keep the M2 storage boundary executable as a continuous build-system gate.
# This test builds only the existing storage target (and its declared common
# dependency), so adding a proxy/driver dependency cannot hide behind the full
# product build.
foreach(_myrd_storage_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_myrd_storage_link_items myrd_storage ${_myrd_storage_link_property})
    if(_myrd_storage_link_items)
        foreach(_myrd_forbidden_storage_dependency IN ITEMS myrd_proxy myrd_driver)
            if(_myrd_forbidden_storage_dependency IN_LIST _myrd_storage_link_items)
                message(FATAL_ERROR
                    "myrd_storage must not depend on ${_myrd_forbidden_storage_dependency}")
            endif()
        endforeach()
    endif()
endforeach()

add_test(
    NAME structural.m2.storage_independent_build
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/support/verify_storage_boundary.ps1"
            -SourceDirectory "${CMAKE_SOURCE_DIR}"
            -ProbeDirectory "${CMAKE_SOURCE_DIR}/out/build/storage-boundary-probe"
            -Configuration "$<CONFIG>"
            -Generator "${CMAKE_GENERATOR}"
            -Compiler "${CMAKE_CXX_COMPILER}")
set_tests_properties(structural.m2.storage_independent_build PROPERTIES
    LABELS "m2-v1;m2-v2;m2-v3;m2-v4"
    RUN_SERIAL TRUE)
