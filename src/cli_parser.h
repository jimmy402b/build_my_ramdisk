#pragma once

#include <string>
#include <cstdint>

enum class Command { None, Create, Mount, Save, Unmount, Status, Tray, Help, Version };

struct CreateOptions {
    uint64_t sizeBytes = 0;
    std::string outputPath;
    bool dynamic = true;
    int saveIntervalSec = 300;
    std::string label = "RAMDISK";
    std::string mountPoint;
    bool daemon = false;
    std::string logFile = "ramdisk-cli.log";
    std::string mode = "vm";
    bool autoFormat = false;
    std::string filesystem = "NTFS";
    bool quickSave = false;
};

struct MountOptions {
    std::string inputPath;
    int saveIntervalSec = 300;
    bool readonly = false;
    std::string label = "RAMDISK";
    bool daemon = false;
    std::string logFile = "ramdisk-cli.log";
    std::string mode = "vm";
    bool quickSave = false;
};

struct SaveOptions    { int device = 0; };
struct UnmountOptions { int device = 0; bool noSave = false; };

struct ParseResult {
    Command command = Command::None;
    CreateOptions create;
    MountOptions mount;
    SaveOptions save;
    UnmountOptions unmount;
    bool verbose = false;
    std::string error;
};

ParseResult ParseArgs(int argc, char* argv[]);
void PrintHelp();
void PrintVersion();
