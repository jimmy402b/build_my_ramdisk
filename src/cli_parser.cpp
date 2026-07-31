#include "cli_parser.h"
#include "utils.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static uint64_t ParseSize(const std::string& s, std::string& error) {
    if (s.empty()) { error = "empty size string"; return 0; }
    char* end = nullptr;
    double num = strtod(s.c_str(), &end);
    if (end == s.c_str()) { error = "invalid size: " + s; return 0; }

    uint64_t multiplier = 1;
    std::string suffix = Trim(std::string(end));
    if (suffix.empty() || suffix == "B")       multiplier = 1;
    else if (suffix == "K" || suffix == "KB")   multiplier = 1024ULL;
    else if (suffix == "M" || suffix == "MB")   multiplier = 1024ULL * 1024;
    else if (suffix == "G" || suffix == "GB")   multiplier = 1024ULL * 1024 * 1024;
    else if (suffix == "T" || suffix == "TB")   multiplier = 1024ULL * 1024 * 1024 * 1024;
    else { error = "unknown size suffix: " + suffix + " (use B/K/M/G/T)"; return 0; }

    return static_cast<uint64_t>(num * multiplier);
}

ParseResult ParseArgs(int argc, char* argv[]) {
    ParseResult r;
    if (argc < 2) { r.command = Command::Help; return r; }

    std::string cmd = argv[1];

    if (cmd == "create") {
        r.command = Command::Create;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if ((arg == "-s" || arg == "--size") && i+1 < argc) {
                std::string err; r.create.sizeBytes = ParseSize(argv[++i], err);
                if (!err.empty()) { r.error = err; return r; }
            } else if ((arg == "-o" || arg == "--output") && i+1 < argc) { r.create.outputPath = argv[++i]; }
            else if ((arg == "-t" || arg == "--type") && i+1 < argc) {
                std::string t = argv[++i];
                if (t == "fixed") r.create.dynamic = false;
                else if (t == "dynamic") r.create.dynamic = true;
                else { r.error = "unknown type: "+t; return r; }
            } else if (arg == "--save-interval" && i+1 < argc) { r.create.saveIntervalSec = std::atoi(argv[++i]); }
            else if (arg == "--label" && i+1 < argc) { r.create.label = argv[++i]; }
            else if (arg == "--mount-point" && i+1 < argc) { r.create.mountPoint = argv[++i]; }
            else if (arg == "--daemon") { r.create.daemon = true; }
            else if (arg == "--log-file" && i+1 < argc) { r.create.logFile = argv[++i]; }
            else if (arg == "--mode" && i+1 < argc) { r.create.mode = argv[++i]; }
            else if (arg == "-v" || arg == "--verbose") { r.verbose = true; }
            else if (arg == "-h" || arg == "--help") { r.command = Command::Help; return r; }
            else { r.error = "unknown option: "+arg; return r; }
        }
        if (r.create.sizeBytes == 0) { r.error = "--size is required for create"; return r; }
        if (r.create.outputPath.empty()) { r.error = "--output is required for create"; return r; }
    }
    else if (cmd == "mount") {
        r.command = Command::Mount;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if ((arg == "-i" || arg == "--input") && i+1 < argc) { r.mount.inputPath = argv[++i]; }
            else if (arg == "--save-interval" && i+1 < argc) { r.mount.saveIntervalSec = std::atoi(argv[++i]); }
            else if (arg == "--readonly") { r.mount.readonly = true; }
            else if (arg == "--label" && i+1 < argc) { r.mount.label = argv[++i]; }
            else if (arg == "--daemon") { r.mount.daemon = true; }
            else if (arg == "--log-file" && i+1 < argc) { r.mount.logFile = argv[++i]; }
            else if (arg == "--mode" && i+1 < argc) { r.mount.mode = argv[++i]; }
            else if (arg == "-v" || arg == "--verbose") { r.verbose = true; }
            else if (arg == "-h" || arg == "--help") { r.command = Command::Help; return r; }
            else { r.error = "unknown option: "+arg; return r; }
        }
        if (r.mount.inputPath.empty()) { r.error = "--input is required for mount"; return r; }
    }
    else if (cmd == "save") {
        r.command = Command::Save;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--device" && i+1 < argc) { r.save.device = std::atoi(argv[++i]); }
            else if (arg == "-v" || arg == "--verbose") { r.verbose = true; }
            else if (arg == "-h" || arg == "--help") { r.command = Command::Help; return r; }
            else { r.error = "unknown option: "+arg; return r; }
        }
    }
    else if (cmd == "unmount") {
        r.command = Command::Unmount;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--device" && i+1 < argc) { r.unmount.device = std::atoi(argv[++i]); }
            else if (arg == "--no-save") { r.unmount.noSave = true; }
            else if (arg == "-v" || arg == "--verbose") { r.verbose = true; }
            else if (arg == "-h" || arg == "--help") { r.command = Command::Help; return r; }
            else { r.error = "unknown option: "+arg; return r; }
        }
    }
    else if (cmd == "status") { r.command = Command::Status; }
    else if (cmd == "help" || cmd == "--help" || cmd == "-h") { r.command = Command::Help; }
    else if (cmd == "version" || cmd == "--version" || cmd == "-V") { r.command = Command::Version; }
    else { r.error = "unknown command: " + cmd; }

    return r;
}

void PrintHelp() {
    printf("ramdisk-cli — RAM disk tool based on Arsenal Image Mounter\n");
    printf("Usage: ramdisk-cli <command> [options]\n\n");
    printf("Commands:\n");
    printf("  create   Create a new VHD and mount as RAM disk\n");
    printf("  mount    Load an existing VHD into RAM\n");
    printf("  save     Manually save RAM disk contents to VHD\n");
    printf("  unmount  Save and unmount a RAM disk\n");
    printf("  status   Show mounted RAM disks\n");
    printf("  help     Show this help\n");
    printf("  version  Show version\n\n");
    printf("Examples:\n");
    printf("  ramdisk-cli create -s 4G -o D:\\disk.vhd --save-interval 600\n");
    printf("  ramdisk-cli mount -i D:\\disk.vhd\n");
    printf("  ramdisk-cli save\n");
    printf("  ramdisk-cli unmount\n");
    printf("  ramdisk-cli status\n");
}

void PrintVersion() { printf("ramdisk-cli v1.0.0\n"); }
