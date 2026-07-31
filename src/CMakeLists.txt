add_executable(ramdisk-cli
    main.cpp
    cli_parser.cpp
    vhd_engine.cpp
    aim_bridge.cpp
    aim_api.cpp
    save_scheduler.cpp
    utils.cpp
    drive_letter.cpp
    daemon.cpp
    dirty_bitmap.cpp
    sector_cache.cpp
    storage_server.cpp
    watchdog.cpp
)

target_include_directories(ramdisk-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# VHD format implemented inline in vhd_engine.cpp — no external dependencies
