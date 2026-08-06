#include "src/m3/m3_windows_acceptance.h"

#include <windows.h>

#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::m3;
using namespace myrd::persistence;

constexpr std::uint64_t kDefaultVirtualBytes = 64ull << 30;
constexpr std::uint64_t kMiB = 1024ull * 1024ull;

void print_error(const Error& error) {
    std::fwprintf(stderr, L"M3 AIM server error (%u): %ls: %ls\n",
        error.code, error.operation.c_str(), error.message.c_str());
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length,
            nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                result += "\\u00";
                constexpr char digits[] = "0123456789abcdef";
                result.push_back(digits[ch >> 4]);
                result.push_back(digits[ch & 0x0f]);
            } else {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return result;
}

bool parse_u64(std::wstring_view value, std::uint64_t& output) {
    if (value.empty()) return false;
    const auto text = narrow(value);
    if (text.empty()) return false;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, output);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool take_option(
    const std::vector<std::wstring_view>& args,
    std::size_t& index,
    std::wstring_view name,
    std::wstring& value) {
    if (args[index] != name || index + 1 >= args.size()) return false;
    value = std::wstring(args[++index]);
    return true;
}

PersistenceLimits default_limits(std::uint64_t virtual_bytes) {
    return PersistenceLimits{
        virtual_bytes,
        512ull * kMiB,
        131072,
        64ull * kMiB,
        65536,
        16ull * kMiB,
        65536,
        8ull * kMiB,
        8ull * kMiB};
}

DynamicBlockStoreOptions default_store_options() {
    return DynamicBlockStoreOptions{
        512ull * kMiB,
        1ull * 1024ull * kMiB,
        256ull * kMiB,
        8ull * kMiB,
        8192,
        256,
        64ull * kMiB,
        std::chrono::milliseconds(250)};
}

void print_ready(const M3AimRuntime& runtime) {
    const auto& info = runtime.info();
    std::cout << "{\"event\":\"ready\",\"device_id\":"
              << info.device_id.value
              << ",\"physical_drive_number\":"
              << info.disk.physical_drive_number
              << ",\"physical_drive_path\":\""
              << json_escape(narrow(info.disk.path))
              << "\",\"virtual_bytes\":" << info.virtual_bytes
              << ",\"sector_size\":" << info.sector_size << "}\n"
              << std::flush;
}

int print_checkpoint(const M3CheckpointOutcome& outcome) {
    std::cout << "{\"event\":\"checkpoint\",\"disposition\":\""
              << persistence::to_string(outcome.disposition)
              << "\",\"generation\":" << outcome.generation
              << ",\"ok\":" << (outcome.ok() ? "true" : "false")
              << "}\n" << std::flush;
    if (outcome.ok()) return 0;
    if (outcome.error.has_value()) print_error(*outcome.error);
    return 8;
}

std::wstring usage() {
    return L"Usage: myrd-m3-aim-server serve --bundle-root DIR "
           L"--manifest NAME [--virtual-bytes N]\n"
           L"Commands on stdin: status | checkpoint volume-flushed | "
           L"checkpoint raw-test | stop\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::wstring_view> args;
        for (int index = 1; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
        if (args.empty() || args.front() == L"--help" ||
            args.front() == L"help") {
            std::wcout << usage();
            return 0;
        }
        if (args.front() != L"serve") {
            std::wcerr << usage();
            return 2;
        }

        std::filesystem::path bundle_root;
        std::string manifest;
        std::uint64_t virtual_bytes = kDefaultVirtualBytes;
        for (std::size_t index = 1; index < args.size(); ++index) {
            std::wstring value;
            if (take_option(args, index, L"--bundle-root", value)) {
                bundle_root = value;
                continue;
            }
            if (take_option(args, index, L"--manifest", value)) {
                manifest = narrow(value);
                continue;
            }
            if (take_option(args, index, L"--virtual-bytes", value)) {
                if (!parse_u64(value, virtual_bytes)) {
                    std::wcerr << L"invalid --virtual-bytes\n";
                    return 2;
                }
                continue;
            }
            std::wcerr << L"unknown or incomplete option: " << args[index]
                       << L"\n";
            return 2;
        }
        if (bundle_root.empty() || manifest.empty() || virtual_bytes == 0) {
            std::wcerr << L"--bundle-root, --manifest and positive geometry "
                          L"are required\n";
            return 2;
        }

        M3AimProviderFactoryConfig config;
        config.context.deadline = std::chrono::steady_clock::now() +
            std::chrono::minutes(5);
        config.provider.bundle_root = std::move(bundle_root);
        config.provider.manifest_name = std::move(manifest);
        config.provider.recovery.limits = default_limits(virtual_bytes);
        config.provider.store = default_store_options();

        auto runtime = M3AimRuntime::create(std::move(config));
        if (!runtime) {
            print_error(runtime.error());
            return 3;
        }
        auto started = runtime.value()->start();
        if (!started) {
            print_error(started.error());
            return 4;
        }
        print_ready(*runtime.value());

        std::string command;
        while (std::getline(std::cin, command)) {
            if (command == "status") {
                print_ready(*runtime.value());
                continue;
            }
            if (command == "checkpoint volume-flushed" ||
                command == "checkpoint raw-test") {
                const auto context = OperationContext{
                    {}, std::chrono::steady_clock::now() +
                        std::chrono::minutes(5)};
                const auto barrier = command == "checkpoint volume-flushed"
                    ? VolumeBarrierKind::WindowsVolumeFlushed
                    : VolumeBarrierKind::RawBlockTestNoFilesystemCache;
                const auto outcome = runtime.value()->checkpoint(
                    context, barrier);
                const int exit_code = print_checkpoint(outcome);
                if (exit_code != 0) {
                    (void)runtime.value()->stop();
                    return exit_code;
                }
                continue;
            }
            if (command == "stop") {
                const auto stopped = runtime.value()->stop();
                if (!stopped) {
                    print_error(stopped.error());
                    return 9;
                }
                std::cout << "{\"event\":\"stopped\",\"ok\":true}\n"
                          << std::flush;
                return 0;
            }
            std::cout << "{\"event\":\"error\",\"reason\":\"unknown_command\"}\n"
                      << std::flush;
            return 7;
        }

        const auto stopped = runtime.value()->stop();
        if (!stopped) {
            print_error(stopped.error());
            return 9;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "M3 AIM server exception: %s\n", error.what());
        return 70;
    } catch (...) {
        std::fputs("M3 AIM server unknown exception\n", stderr);
        return 71;
    }
}
