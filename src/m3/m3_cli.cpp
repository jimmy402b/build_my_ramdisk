#include "src/m3/m3_cli.h"

#include "src/persistence/image_paths.h"
#include "src/persistence/vhdx_reader.h"

#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace myrd::m3 {
namespace {

Error usage_error(const wchar_t* message) {
    return {ErrorDomain::usage, kM3CliUsageErrorCode, L"parse_m3_cli",
            message};
}

Result<std::uint64_t> parse_u64(const std::wstring_view text) {
    if (text.empty()) {
        return Result<std::uint64_t>::failure(
            usage_error(L"numeric option requires a value"));
    }
    std::uint64_t value{};
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return Result<std::uint64_t>::failure(
                usage_error(L"numeric option must be unsigned decimal"));
        }
        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) /
                         10) {
            return Result<std::uint64_t>::failure(
                usage_error(L"numeric option overflows uint64"));
        }
        value = value * 10 + digit;
    }
    return Result<std::uint64_t>::success(value);
}

Result<std::string> parse_manifest(const std::wstring_view text) {
    if (text.empty()) {
        return Result<std::string>::failure(
            usage_error(L"manifest is required"));
    }
    std::string value;
    value.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch < 0x20 || ch > 0x7e) {
            return Result<std::string>::failure(
                usage_error(L"manifest must be printable ASCII"));
        }
        value.push_back(static_cast<char>(ch));
    }
    if (!persistence::validate_image_component(value).ok()) {
        return Result<std::string>::failure(
            usage_error(L"manifest must be one safe bundle component"));
    }
    return Result<std::string>::success(std::move(value));
}

Result<std::filesystem::path> parse_bundle_root(
    const std::wstring_view text) {
    if (text.empty() || text.find(L'\0') != std::wstring_view::npos ||
        text.starts_with(L"\\\\.\\") ||
        text.starts_with(L"\\\\?\\Volume{") ||
        text.starts_with(L"\\\\?\\GLOBALROOT\\")) {
        return Result<std::filesystem::path>::failure(
            usage_error(L"bundle root must be a non-device path"));
    }
    return Result<std::filesystem::path>::success(
        std::filesystem::path(std::wstring(text)));
}

struct Seen final {
    bool root{}, manifest{}, timeout{};
    bool virtual_bytes{}, logical_sector{}, physical_sector{};
    bool barrier{}, full{}, phase{};
    bool max_virtual{}, max_journal{}, max_records{}, max_index_bytes{};
    bool max_index_entries{}, max_bat_bytes{}, max_bat_entries{};
    bool max_scratch{}, max_verify{};
    bool store_soft{}, store_hard{}, store_low{}, store_batch_bytes{};
    bool store_batch_records{}, store_stripes{}, cow_bytes{}, stall{};

    [[nodiscard]] bool all_persistence() const noexcept {
        return max_virtual && max_journal && max_records && max_index_bytes &&
            max_index_entries && max_bat_bytes && max_bat_entries &&
            max_scratch && max_verify;
    }
    [[nodiscard]] bool all_store() const noexcept {
        return store_soft && store_hard && store_low && store_batch_bytes &&
            store_batch_records && store_stripes && cow_bytes && stall;
    }
    [[nodiscard]] bool any_store() const noexcept {
        return store_soft || store_hard || store_low || store_batch_bytes ||
            store_batch_records || store_stripes || cow_bytes || stall;
    }
};

template <typename ParseValue, typename Assign>
Result<void> option_value(std::span<const std::wstring_view> args,
                          std::size_t& index,
                          bool& seen,
                          const wchar_t* option,
                          ParseValue&& parse,
                          Assign&& assign) {
    if (seen) {
        return Result<void>::failure(usage_error(L"duplicate CLI option"));
    }
    if (index + 1 >= args.size()) {
        return Result<void>::failure(usage_error(option));
    }
    ++index;
    auto value = parse(args[index]);
    if (!value.ok()) return Result<void>::failure(value.error());
    auto parsed_value = value.move_value();
    if constexpr (std::is_same_v<
                      std::invoke_result_t<Assign, decltype(parsed_value)>,
                      bool>) {
        if (!assign(std::move(parsed_value))) {
            return Result<void>::failure(usage_error(option));
        }
    } else {
        assign(std::move(parsed_value));
    }
    seen = true;
    return Result<void>::success();
}

template <typename Assign>
Result<void> number_option(std::span<const std::wstring_view> args,
                           std::size_t& index,
                           bool& seen,
                           const wchar_t* option,
                           Assign&& assign) {
    return option_value(args, index, seen, option,
                        [](std::wstring_view text) { return parse_u64(text); },
                        std::forward<Assign>(assign));
}

bool requires_store(const M3CliCommand command) noexcept {
    return command == M3CliCommand::Checkpoint ||
        command == M3CliCommand::Compact ||
        command == M3CliCommand::RecoverTest ||
        command == M3CliCommand::FaultMatrix;
}

} // namespace

Result<M3CliCommand> parse_m3_cli_command(const std::string_view value) {
    for (std::uint8_t raw = static_cast<std::uint8_t>(M3CliCommand::Create);
         raw <= static_cast<std::uint8_t>(M3CliCommand::FaultMatrix); ++raw) {
        const auto command = static_cast<M3CliCommand>(raw);
        if (value == command_name(command)) {
            return Result<M3CliCommand>::success(command);
        }
    }
    return Result<M3CliCommand>::failure(usage_error(L"unknown M3 command"));
}

Result<M3CliConfig> parse_m3_cli(
    const std::span<const std::wstring_view> args) {
    if (args.empty()) {
        return Result<M3CliConfig>::failure(
            usage_error(L"an explicit M3 command is required"));
    }
    if (args.front() == L"--help") {
        if (args.size() != 1) {
            return Result<M3CliConfig>::failure(
                usage_error(L"--help cannot be combined with other arguments"));
        }
        M3CliConfig help{};
        help.command = M3CliCommand::Help;
        return Result<M3CliConfig>::success(std::move(help));
    }

    std::string command_text;
    command_text.reserve(args.front().size());
    for (const wchar_t ch : args.front()) {
        if (ch > 0x7f) {
            return Result<M3CliConfig>::failure(
                usage_error(L"command must be ASCII"));
        }
        command_text.push_back(static_cast<char>(ch));
    }
    auto command = parse_m3_cli_command(command_text);
    if (!command.ok()) return Result<M3CliConfig>::failure(command.error());

    M3CliConfig result{};
    result.command = command.value();
    Seen seen{};
    DynamicBlockStoreOptions store{};

    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto option = args[index];
        if (option == L"--bundle-root") {
            auto parsed = option_value(
                args, index, seen.root, L"--bundle-root requires a value",
                [](std::wstring_view text) { return parse_bundle_root(text); },
                [&result](std::filesystem::path value) {
                    result.bundle_root = std::move(value);
                });
            if (!parsed.ok()) return Result<M3CliConfig>::failure(parsed.error());
        } else if (option == L"--manifest") {
            auto parsed = option_value(
                args, index, seen.manifest, L"--manifest requires a value",
                [](std::wstring_view text) { return parse_manifest(text); },
                [&result](std::string value) {
                    result.manifest_name = std::move(value);
                });
            if (!parsed.ok()) return Result<M3CliConfig>::failure(parsed.error());
        } else if (option == L"--timeout-ms") {
            auto parsed = number_option(
                args, index, seen.timeout, L"--timeout-ms requires a value",
                [&result](std::uint64_t value) {
                    if (value == 0 || value > 24ull * 60ull * 60ull * 1000ull ||
                        value > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::int64_t>::max)())) {
                        return false;
                    }
                    result.timeout = std::chrono::milliseconds(
                        static_cast<std::int64_t>(value));
                    return true;
                });
            if (!parsed.ok()) return Result<M3CliConfig>::failure(parsed.error());
            if (result.timeout.count() == 0) return Result<M3CliConfig>::failure(
                usage_error(L"timeout must be between 1 ms and 24 hours"));
        } else if (option == L"--virtual-bytes" ||
                   option == L"--logical-sector-size" ||
                   option == L"--physical-sector-size") {
            auto* flag = option == L"--virtual-bytes"
                ? &seen.virtual_bytes
                : option == L"--logical-sector-size"
                    ? &seen.logical_sector : &seen.physical_sector;
            auto parsed = number_option(
                args, index, *flag, L"geometry option requires a value",
                [&result, option](std::uint64_t value) {
                    if (!result.geometry.has_value()) result.geometry.emplace();
                    if (option == L"--virtual-bytes") {
                        result.geometry->virtual_bytes = value;
                    } else if (option == L"--logical-sector-size") {
                        if (value > (std::numeric_limits<std::uint32_t>::max)())
                            return false;
                        result.geometry->logical_sector_size =
                            static_cast<std::uint32_t>(value);
                    } else {
                        if (value > (std::numeric_limits<std::uint32_t>::max)())
                            return false;
                        result.geometry->physical_sector_size =
                            static_cast<std::uint32_t>(value);
                    }
                    return true;
                });
            if (!parsed.ok()) return Result<M3CliConfig>::failure(parsed.error());
        } else if (option == L"--barrier") {
            auto parsed = option_value(
                args, index, seen.barrier, L"--barrier requires raw-test",
                [](std::wstring_view text)
                    -> Result<persistence::VolumeBarrierKind> {
                    if (text != L"raw-test") return Result<persistence::VolumeBarrierKind>::failure(
                        usage_error(L"local CLI barrier must be raw-test"));
                    return Result<persistence::VolumeBarrierKind>::success(
                        persistence::VolumeBarrierKind::RawBlockTestNoFilesystemCache);
                },
                [&result](persistence::VolumeBarrierKind value) {
                    result.barrier = value;
                });
            if (!parsed.ok()) return Result<M3CliConfig>::failure(parsed.error());
        } else if (option == L"--full") {
            if (seen.full) return Result<M3CliConfig>::failure(
                usage_error(L"duplicate CLI option"));
            seen.full = true;
            result.full_verify = true;
        } else if (option == L"--inject-phase") {
            auto parsed = option_value(
                args, index, seen.phase, L"--inject-phase requires a value",
                [](std::wstring_view text) -> Result<std::string> {
                    if (text.empty() || text.size() > 128) return
                        Result<std::string>::failure(
                            usage_error(L"injected phase must be bounded"));
                    std::string value;
                    value.reserve(text.size());
                    for (const wchar_t ch : text) {
                        if (ch < 0x21 || ch > 0x7e) return
                            Result<std::string>::failure(
                                usage_error(L"injected phase must be ASCII"));
                        value.push_back(static_cast<char>(ch));
                    }
                    return Result<std::string>::success(std::move(value));
                },
                [&result](std::string value) {
                    result.injected_phase = std::move(value);
                });
            if (!parsed.ok()) return Result<M3CliConfig>::failure(parsed.error());
        } else {
            bool recognized = true;
            Result<void> parsed = Result<void>::success();
            auto assign_limit = [&](bool& flag, std::uint64_t& destination) {
                return number_option(args, index, flag, L"limit requires a value",
                    [&destination](std::uint64_t value) {
                        destination = value;
                    });
            };
            if (option == L"--max-virtual-bytes") parsed = assign_limit(
                seen.max_virtual, result.persistence_limits.max_virtual_bytes);
            else if (option == L"--max-journal-bytes") parsed = assign_limit(
                seen.max_journal,
                result.persistence_limits.max_journal_committed_bytes);
            else if (option == L"--max-journal-records") parsed = assign_limit(
                seen.max_records, result.persistence_limits.max_journal_records);
            else if (option == L"--max-index-cache-bytes") parsed = assign_limit(
                seen.max_index_bytes,
                result.persistence_limits.max_index_cache_bytes);
            else if (option == L"--max-index-cache-entries") parsed = assign_limit(
                seen.max_index_entries,
                result.persistence_limits.max_index_cache_entries);
            else if (option == L"--max-bat-cache-bytes") parsed = assign_limit(
                seen.max_bat_bytes,
                result.persistence_limits.max_bat_cache_bytes);
            else if (option == L"--max-bat-cache-entries") parsed = assign_limit(
                seen.max_bat_entries,
                result.persistence_limits.max_bat_cache_entries);
            else if (option == L"--max-io-scratch-bytes") parsed = assign_limit(
                seen.max_scratch, result.persistence_limits.max_io_scratch_bytes);
            else if (option == L"--max-verify-batch-bytes") parsed = assign_limit(
                seen.max_verify, result.persistence_limits.max_verify_batch_bytes);
            else if (option == L"--store-soft-bytes") parsed = assign_limit(
                seen.store_soft, store.soft_limit_bytes);
            else if (option == L"--store-hard-bytes") parsed = assign_limit(
                seen.store_hard, store.hard_limit_bytes);
            else if (option == L"--store-low-watermark-bytes") parsed = assign_limit(
                seen.store_low, store.eviction_low_watermark_bytes);
            else if (option == L"--store-batch-bytes") parsed = assign_limit(
                seen.store_batch_bytes, store.max_snapshot_batch_bytes);
            else if (option == L"--store-batch-records") {
                parsed = number_option(args, index, seen.store_batch_records,
                    L"store batch records requires a value",
                    [&store](std::uint64_t value) {
                        if (value == (std::numeric_limits<std::uint64_t>::max)() ||
                            value > (std::numeric_limits<std::size_t>::max)())
                            return false;
                        store.max_snapshot_batch_records =
                            static_cast<std::size_t>(value);
                        return true;
                    });
            } else if (option == L"--store-stripes") {
                parsed = number_option(args, index, seen.store_stripes,
                    L"store stripes requires a value",
                    [&store](std::uint64_t value) {
                        if (value == (std::numeric_limits<std::uint64_t>::max)() ||
                            value > (std::numeric_limits<std::size_t>::max)())
                            return false;
                        store.lock_stripe_count = static_cast<std::size_t>(value);
                        return true;
                    });
            } else if (option == L"--snapshot-cow-bytes") parsed = assign_limit(
                seen.cow_bytes, store.snapshot_cow_max_bytes);
            else if (option == L"--snapshot-stall-ms") {
                parsed = number_option(args, index, seen.stall,
                    L"snapshot stall requires a value",
                    [&store](std::uint64_t value) {
                        if (value == 0 || value > 5000 ||
                            value > static_cast<std::uint64_t>(
                                (std::numeric_limits<std::int64_t>::max)()))
                            return false;
                        store.snapshot_writer_stall_limit =
                            std::chrono::milliseconds(
                                static_cast<std::int64_t>(value));
                        return true;
                    });
            } else {
                recognized = false;
            }
            if (!recognized) return Result<M3CliConfig>::failure(
                usage_error(L"unknown M3 option"));
            if (!parsed.ok()) return Result<M3CliConfig>::failure(parsed.error());
        }
    }

    if (!seen.root || !seen.manifest || !seen.timeout ||
        !seen.all_persistence()) {
        return Result<M3CliConfig>::failure(
            usage_error(L"common path, timeout and finite limits are required"));
    }
    if (persistence::validate_limits(result.persistence_limits) !=
        persistence::LimitsStatus::Ok) {
        return Result<M3CliConfig>::failure(
            usage_error(L"persistence limits are invalid"));
    }
    if (result.persistence_limits.max_bat_cache_entries >
        result.persistence_limits.max_bat_cache_bytes /
            persistence::kVhdxBatPageBytes) {
        return Result<M3CliConfig>::failure(
            usage_error(L"BAT cache entries exceed the VHDX page budget"));
    }
    if (result.command == M3CliCommand::Create) {
        if (!seen.virtual_bytes || !seen.logical_sector ||
            !seen.physical_sector || !result.geometry.has_value() ||
            persistence::validate_geometry(*result.geometry) !=
                persistence::GeometryStatus::Ok ||
            result.geometry->virtual_bytes >
                result.persistence_limits.max_virtual_bytes) {
            return Result<M3CliConfig>::failure(
                usage_error(L"create requires valid explicit geometry"));
        }
    } else if (seen.virtual_bytes || seen.logical_sector ||
               seen.physical_sector) {
        return Result<M3CliConfig>::failure(
            usage_error(L"geometry is only valid for create"));
    }

    if (requires_store(result.command)) {
        const auto finite_store_limits =
            store.soft_limit_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
            store.hard_limit_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
            store.eviction_low_watermark_bytes !=
                (std::numeric_limits<std::uint64_t>::max)() &&
            store.max_snapshot_batch_bytes !=
                (std::numeric_limits<std::uint64_t>::max)() &&
            store.snapshot_cow_max_bytes !=
                (std::numeric_limits<std::uint64_t>::max)();
        if (!seen.all_store() || !finite_store_limits ||
            !validate_dynamic_block_store_options(store).ok()) {
            return Result<M3CliConfig>::failure(
                usage_error(L"this command requires valid explicit store limits"));
        }
        result.store_options = store;
    } else if (seen.any_store()) {
        return Result<M3CliConfig>::failure(
            usage_error(L"store limits are not valid for this command"));
    }

    const bool mutating = result.command == M3CliCommand::Checkpoint ||
        result.command == M3CliCommand::Compact;
    if (mutating != seen.barrier) {
        return Result<M3CliConfig>::failure(
            usage_error(L"checkpoint and compact require raw-test barrier"));
    }
    if (seen.full && result.command != M3CliCommand::Verify) {
        return Result<M3CliConfig>::failure(
            usage_error(L"--full is only valid for verify"));
    }
    if (seen.phase && result.command != M3CliCommand::RecoverTest &&
        result.command != M3CliCommand::FaultMatrix) {
        return Result<M3CliConfig>::failure(
            usage_error(L"phase injection is test-command only"));
    }
    if (result.command == M3CliCommand::FaultMatrix && !seen.phase) {
        return Result<M3CliConfig>::failure(
            usage_error(L"fault-matrix requires explicit phase injection"));
    }
    return Result<M3CliConfig>::success(std::move(result));
}

std::wstring_view m3_cli_usage() noexcept {
    static constexpr std::wstring_view usage =
        L"Usage: myrd-m3-e2e <create|inspect|verify|checkpoint|compact|recover-test|fault-matrix>\n"
        L"  --bundle-root DIR --manifest NAME --timeout-ms N\n"
        L"  finite limits: --max-virtual-bytes N --max-journal-bytes N\n"
        L"    --max-journal-records N --max-index-cache-bytes N\n"
        L"    --max-index-cache-entries N --max-bat-cache-bytes N\n"
        L"    --max-bat-cache-entries N --max-io-scratch-bytes N\n"
        L"    --max-verify-batch-bytes N\n"
        L"  create: --virtual-bytes N --logical-sector-size N --physical-sector-size N\n"
        L"  checkpoint/compact: --barrier raw-test\n"
        L"  checkpoint/compact/recover-test/fault-matrix: explicit store limits\n"
        L"  verify: [--full]; recover-test/fault-matrix: --inject-phase TOKEN\n";
    return usage;
}

} // namespace myrd::m3
