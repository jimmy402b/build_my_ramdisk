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

include(CTest)
add_test(NAME unit.common COMMAND myrd_common_tests)
add_test(NAME unit.protocol COMMAND myrd_protocol_tests)

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
