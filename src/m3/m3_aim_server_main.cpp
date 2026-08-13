#include "src/m3/m3_windows_acceptance.h"
#include "src/persistence/vhdx_reader.h"
#if defined(MYRD_ENABLE_TEST_SEAMS)
#include "src/m3/m3_crash_test_control.h"
#endif

#include <windows.h>

#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
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

std::string image_uuid_hex(const ImageUuid& uuid) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(uuid.bytes.size() * 2);
    for (const auto value : uuid.bytes) {
        const auto byte = std::to_integer<unsigned char>(value);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
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
    return production_persistence_limits(virtual_bytes);
}

DynamicBlockStoreOptions default_store_options() {
    return production_store_options(1ull * 1024ull * kMiB);
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

void print_status(const M3AimRuntime& runtime) {
    const auto& info = runtime.info();
    const auto storage = runtime.storage_stats();
    const auto transport = runtime.transport_snapshot();
    const auto& requests = transport.requests;
    std::cout << "{\"event\":\"status\",\"device_id\":"
              << info.device_id.value
              << ",\"physical_drive_number\":"
              << info.disk.physical_drive_number
              << ",\"storage\":{"
              << "\"logical_size_bytes\":" << storage.logical_size_bytes
              << ",\"committed_bytes\":" << storage.committed_bytes
              << ",\"resident_clean_blocks\":"
              << storage.resident_clean_blocks
              << ",\"resident_dirty_blocks\":"
              << storage.resident_dirty_blocks
              << ",\"zero_dirty_blocks\":"
              << storage.zero_dirty_blocks
              << ",\"zero_clean_blocks\":"
              << storage.zero_clean_blocks
              << ",\"backing_reads_total\":"
              << storage.backing_reads_total
              << ",\"backing_read_bytes_total\":"
              << storage.backing_read_bytes_total
              << ",\"uncached_reads_total\":"
              << storage.uncached_reads_total
              << ",\"evicted_blocks_total\":"
              << storage.evicted_blocks_total
              << ",\"backing_generation\":"
              << storage.backing_generation
              << ",\"snapshot_cow_current_bytes\":"
              << storage.snapshot_cow_current_bytes
              << ",\"snapshot_cow_peak_bytes\":"
              << storage.snapshot_cow_peak_bytes
              << "},\"transport\":{"
              << "\"info\":" << requests.info
              << ",\"read\":" << requests.read
              << ",\"write\":" << requests.write
              << ",\"unmap\":" << requests.unmap
              << ",\"zero\":" << requests.zero
              << ",\"close\":" << requests.close
              << ",\"read_errors\":" << requests.read_errors
              << ",\"write_errors\":" << requests.write_errors
              << "}}\n" << std::flush;
}

int print_checkpoint(const M3CheckpointOutcome& outcome) {
    std::cout << "{\"event\":\"checkpoint\",\"disposition\":\""
              << persistence::to_string(outcome.disposition)
              << "\",\"generation\":" << outcome.generation
              << ",\"ok\":" << (outcome.ok() ? "true" : "false");
    if (outcome.error.has_value()) {
        const auto& error = *outcome.error;
        std::cout << ",\"error\":{\"code\":" << error.code
                  << ",\"native_code\":" << error.native_code
                  << ",\"operation\":\""
                  << json_escape(narrow(error.operation))
                  << "\",\"message\":\""
                  << json_escape(narrow(error.message)) << "\"";
        if (outcome.error_context.has_value()) {
            const auto& context = *outcome.error_context;
            std::cout << ",\"phase\":\""
                      << persistence::to_string(context.phase)
                      << "\",\"image_uuid\":\""
                      << image_uuid_hex(context.image_uuid)
                      << "\",\"operation_id\":" << context.operation_id
                      << ",\"generation\":" << context.generation
                      << ",\"disposition\":\""
                      << persistence::to_string(context.disposition)
                      << "\",\"safe_context\":\""
                      << json_escape(context.safe_context) << "\"";
        }
        std::cout << "}";
    }
    std::cout << "}\n" << std::flush;
    if (outcome.ok()) return 0;
    if (outcome.error.has_value()) print_error(*outcome.error);
    return 8;
}

std::wstring usage() {
    return L"Usage: myrd-m3-aim-server serve --bundle-root DIR "
           L"--manifest NAME [--virtual-bytes N]\n"
#if defined(MYRD_ENABLE_TEST_SEAMS)
           L"Test build only: --test-crash-phase NAME "
           L"--test-crash-signal FILE --test-crash-release FILE "
           L"[--test-crash-timeout-ms N]\n"
#endif
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
#if defined(MYRD_ENABLE_TEST_SEAMS)
        std::wstring crash_phase_name;
        std::filesystem::path crash_signal;
        std::filesystem::path crash_release;
        std::uint64_t crash_timeout_ms = 10ull * 60ull * 1000ull;
#endif
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
#if defined(MYRD_ENABLE_TEST_SEAMS)
            if (take_option(args, index, L"--test-crash-phase", value)) {
                crash_phase_name = std::move(value);
                continue;
            }
            if (take_option(args, index, L"--test-crash-signal", value)) {
                crash_signal = std::move(value);
                continue;
            }
            if (take_option(args, index, L"--test-crash-release", value)) {
                crash_release = std::move(value);
                continue;
            }
            if (take_option(
                    args, index, L"--test-crash-timeout-ms", value)) {
                if (!parse_u64(value, crash_timeout_ms) ||
                    crash_timeout_ms == 0 || crash_timeout_ms > 3600000) {
                    std::wcerr << L"invalid --test-crash-timeout-ms\n";
                    return 2;
                }
                continue;
            }
#endif
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

#if defined(MYRD_ENABLE_TEST_SEAMS)
        const bool any_crash_option = !crash_phase_name.empty() ||
            !crash_signal.empty() || !crash_release.empty();
        const bool all_crash_options = !crash_phase_name.empty() &&
            !crash_signal.empty() && !crash_release.empty();
        if (any_crash_option != all_crash_options) {
            std::wcerr << L"all crash-test options must be supplied together\n";
            return 2;
        }
        if (all_crash_options) {
            const auto phase = parse_m3_crash_test_phase(
                narrow(crash_phase_name));
            if (!phase.ok()) {
                print_error(phase.error());
                return 2;
            }
            auto pause = M3CrashTestPauseController::create(
                phase.value(), std::move(crash_signal),
                std::move(crash_release),
                std::chrono::milliseconds(crash_timeout_ms));
            if (!pause.ok()) {
                print_error(pause.error());
                return 2;
            }
            auto controller = std::make_shared<M3CrashTestPauseController>(
                pause.move_value());
            config.test_factories.before_checkpoint_prepare =
                [controller] {
                    return controller->observe(
                        M3CrashTestPhase::CheckpointPrepareBefore);
                };
            config.test_factories.journal_phase_hook =
                [controller](const JournalWriterPhaseEvent& event) {
                    const auto mapped = map_m3_crash_test_phase(event.phase);
                    return mapped.has_value()
                        ? controller->observe(*mapped)
                        : Result<void>::success();
                };
            config.test_factories.manifest_phase_hook =
                [controller](const ManifestPublishTestPhase phase_value) {
                    const auto mapped = map_m3_crash_test_phase(phase_value);
                    return mapped.has_value()
                        ? controller->observe(*mapped)
                        : Result<void>::success();
                };
            config.test_factories.reconcile_phase_hook =
                [controller](
                    const CheckpointReconcileTestPhase phase_value) {
                    const auto mapped = map_m3_crash_test_phase(phase_value);
                    return mapped.has_value()
                        ? controller->observe(*mapped)
                        : Result<void>::success();
                };
        }
#endif

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
                print_status(*runtime.value());
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
