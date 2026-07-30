#pragma once

#include <string>
#include <cstdint>

enum class Command { None, Create, Mount, Save, Unmount, Status, Help, Version };

struct CreateOptions {
    uint64_t sizeBytes = 0;
    std::string outputPath;
    bool dynamic = true;
    int saveIntervalSec = 300;
    std::string label = "RAMDISK";
    std::string mountPoint;
};

struct MountOptions {
    std::string inputPath;
    int saveIntervalSec = 300;
    bool readonly = false;
    std::string label = "RAMDISK";
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
