add_executable(ramdisk-cli
    main.cpp
    cli_parser.cpp
    vhd_engine.cpp
    aim_bridge.cpp
    save_scheduler.cpp
    utils.cpp
)

target_include_directories(ramdisk-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# VHD format implemented inline in vhd_engine.cpp — no external dependencies
