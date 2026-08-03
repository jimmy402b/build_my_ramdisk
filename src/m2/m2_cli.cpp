#include "m2_cli.h"

#include "src/storage/dynamic_block_types.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <system_error>

namespace myrd {
namespace {

constexpr std::uint64_t kRealDiskSizeBytes = 64ull << 30;

Result<M2CliConfig> usage_error(std::wstring message) {
    return Result<M2CliConfig>::failure({
        ErrorDomain::usage, 2, L"parse_m2_cli", std::move(message)});
}

bool is_device_namespace(const std::filesystem::path& path) {
    const auto value = path.wstring();
    return value.starts_with(L"\\\\.\\") ||
        value.starts_with(L"\\\\?\\");
}

Result<std::filesystem::path> existing_path(
    std::wstring_view text,
    bool require_directory) {
    const std::filesystem::path input(text);
    if (input.empty() || is_device_namespace(input)) {
        return Result<std::filesystem::path>::failure({
            ErrorDomain::usage, 2, L"parse_m2_cli",
            L"path is empty or names a device namespace"});
    }
    std::error_code error;
    const auto resolved = std::filesystem::canonical(input, error);
    if (error) {
        return Result<std::filesystem::path>::failure({
            ErrorDomain::usage, 2, L"parse_m2_cli",
            L"path does not exist or cannot be resolved"});
    }
    const bool correct_type = require_directory
        ? std::filesystem::is_directory(resolved, error)
        : std::filesystem::is_regular_file(resolved, error);
    if (error || !correct_type) {
        return Result<std::filesystem::path>::failure({
            ErrorDomain::usage, 2, L"parse_m2_cli",
            require_directory ? L"path is not a directory"
                              : L"path is not a regular file"});
    }
    return Result<std::filesystem::path>::success(resolved);
}

Result<std::uint64_t> parse_uint64(std::wstring_view text) {
    if (text.empty()) {
        return Result<std::uint64_t>::failure({
            ErrorDomain::usage, 2, L"parse_m2_cli",
            L"numeric argument is empty"});
    }
    std::uint64_t value = 0;
    for (const auto current : text) {
        if (current < L'0' || current > L'9') {
            return Result<std::uint64_t>::failure({
                ErrorDomain::usage, 2, L"parse_m2_cli",
                L"numeric argument is not an unsigned decimal integer"});
        }
        const auto digit = static_cast<std::uint64_t>(current - L'0');
        if (value >
            ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10) {
            return Result<std::uint64_t>::failure({
                ErrorDomain::usage, 2, L"parse_m2_cli",
                L"numeric argument overflows uint64"});
        }
        value = value * 10 + digit;
    }
    return Result<std::uint64_t>::success(value);
}

Result<std::string> parse_ascii_identity(std::wstring_view text) {
    if (text.empty() || text.size() > 128) {
        return Result<std::string>::failure({
            ErrorDomain::usage, 2, L"parse_m2_cli",
            L"identity argument is empty or too long"});
    }
    std::string result;
    result.reserve(text.size());
    for (const auto current : text) {
        if (current < 0x21 || current > 0x7e) {
            return Result<std::string>::failure({
                ErrorDomain::usage, 2, L"parse_m2_cli",
                L"identity argument must be printable ASCII without spaces"});
        }
        result.push_back(static_cast<char>(current));
    }
    return Result<std::string>::success(std::move(result));
}

Result<std::string> parse_sha256(std::wstring_view text) {
    if (text.size() != 64) {
        return Result<std::string>::failure({
            ErrorDomain::usage, 2, L"parse_m2_cli",
            L"driver SHA-256 must contain exactly 64 hexadecimal digits"});
    }
    std::string result;
    result.reserve(text.size());
    for (const auto current : text) {
        if (!((current >= L'0' && current <= L'9') ||
              (current >= L'a' && current <= L'f') ||
              (current >= L'A' && current <= L'F'))) {
            return Result<std::string>::failure({
                ErrorDomain::usage, 2, L"parse_m2_cli",
                L"driver SHA-256 contains a non-hexadecimal character"});
        }
        result.push_back(static_cast<char>(
            current >= L'A' && current <= L'F'
                ? current - L'A' + L'a'
                : current));
    }
    return Result<std::string>::success(std::move(result));
}

struct Seen {
    bool evidence{};
    bool report{};
    bool accepted_m1{};
    bool driver_service{};
    bool driver_hash{};
    bool bus{};
    bool vendor{};
    bool product{};
    bool disk_size{};
    bool test_policy{};
    bool soft{};
    bool hard{};
    bool low{};
    bool snapshot_bytes{};
    bool snapshot_records{};
    bool stripes{};
    bool safety{};
};

bool any_real_option(const Seen& seen) {
    return seen.accepted_m1 || seen.driver_service || seen.driver_hash ||
        seen.bus || seen.vendor || seen.product || seen.disk_size ||
        seen.test_policy || seen.soft || seen.hard || seen.low ||
        seen.snapshot_bytes || seen.snapshot_records || seen.stripes ||
        seen.safety;
}

template <typename Setter>
Result<void> parse_value_option(
    std::span<const std::wstring_view> args,
    std::size_t& index,
    bool& seen,
    std::wstring_view option,
    Setter&& setter) {
    if (seen) {
        return Result<void>::failure({
            ErrorDomain::usage, 2, L"parse_m2_cli",
            L"duplicate option"});
    }
    if (++index >= args.size()) {
        return Result<void>::failure({
            ErrorDomain::usage, 2, L"parse_m2_cli",
            std::wstring(option) + L" requires a value"});
    }
    auto assigned = setter(args[index]);
    if (!assigned) return assigned;
    seen = true;
    return Result<void>::success();
}

} // namespace

Result<M2CliConfig> parse_m2_cli(
    std::span<const std::wstring_view> args) {
    M2CliConfig result;
    result.geometry.sector_size_bytes = 512;
    result.geometry.block_size_bytes = kBlockSizeBytes;
    result.geometry.backing_kind = "zero";

    Seen seen;
    std::optional<M2CliMode> mode;
    const auto set_mode = [&mode](M2CliMode value) -> Result<void> {
        if (mode) {
            return Result<void>::failure({
                ErrorDomain::usage, 2, L"parse_m2_cli",
                L"exactly one CLI mode is allowed"});
        }
        mode = value;
        return Result<void>::success();
    };

    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto argument = args[index];
        Result<void> parsed = Result<void>::success();
        if (argument == L"--help") {
            parsed = set_mode(M2CliMode::help);
        } else if (argument == L"--print-schema") {
            parsed = set_mode(M2CliMode::print_schema);
        } else if (argument == L"--local-fake") {
            parsed = set_mode(M2CliMode::local_fake);
        } else if (argument == L"--real") {
            parsed = set_mode(M2CliMode::real);
        } else if (argument == L"--validate-report") {
            parsed = set_mode(M2CliMode::validate_report);
            if (parsed) {
                parsed = parse_value_option(
                    args, index, seen.report, argument,
                    [&result](std::wstring_view value) {
                        auto path = existing_path(value, false);
                        if (!path) return Result<void>::failure(path.error());
                        result.report_to_validate = path.move_value();
                        return Result<void>::success();
                    });
            }
        } else if (argument == L"--evidence-dir") {
            parsed = parse_value_option(
                args, index, seen.evidence, argument,
                [&result](std::wstring_view value) {
                    auto path = existing_path(value, true);
                    if (!path) return Result<void>::failure(path.error());
                    result.evidence_directory = path.move_value();
                    return Result<void>::success();
                });
        } else if (argument == L"--accepted-m1-report") {
            parsed = parse_value_option(
                args, index, seen.accepted_m1, argument,
                [&result](std::wstring_view value) {
                    auto path = existing_path(value, false);
                    if (!path) return Result<void>::failure(path.error());
                    result.accepted_m1_report = path.move_value();
                    return Result<void>::success();
                });
        } else if (argument == L"--expected-driver-service") {
            parsed = parse_value_option(
                args, index, seen.driver_service, argument,
                [&result](std::wstring_view value) {
                    auto identity = parse_ascii_identity(value);
                    if (!identity) {
                        return Result<void>::failure(identity.error());
                    }
                    result.expected_driver_service = identity.move_value();
                    return Result<void>::success();
                });
        } else if (argument == L"--expected-driver-sha256") {
            parsed = parse_value_option(
                args, index, seen.driver_hash, argument,
                [&result](std::wstring_view value) {
                    auto hash = parse_sha256(value);
                    if (!hash) return Result<void>::failure(hash.error());
                    result.expected_driver_sha256 = hash.move_value();
                    return Result<void>::success();
                });
        } else if (argument == L"--expected-bus-type" ||
                   argument == L"--expected-vendor-id" ||
                   argument == L"--expected-product-id") {
            bool* option_seen = argument == L"--expected-bus-type"
                ? &seen.bus
                : argument == L"--expected-vendor-id" ? &seen.vendor
                                                        : &seen.product;
            std::string* destination = argument == L"--expected-bus-type"
                ? &result.expected_bus_type
                : argument == L"--expected-vendor-id"
                    ? &result.expected_vendor_id
                    : &result.expected_product_id;
            parsed = parse_value_option(
                args, index, *option_seen, argument,
                [destination](std::wstring_view value) {
                    auto identity = parse_ascii_identity(value);
                    if (!identity) {
                        return Result<void>::failure(identity.error());
                    }
                    *destination = identity.move_value();
                    return Result<void>::success();
                });
        } else if (argument == L"--disk-size-bytes" ||
                   argument == L"--soft-limit-bytes" ||
                   argument == L"--hard-limit-bytes" ||
                   argument == L"--low-watermark-bytes" ||
                   argument == L"--max-snapshot-batch-bytes" ||
                   argument == L"--max-snapshot-batch-records" ||
                   argument == L"--lock-stripe-count") {
            bool* option_seen = nullptr;
            std::uint64_t* destination = nullptr;
            if (argument == L"--disk-size-bytes") {
                option_seen = &seen.disk_size;
                destination = &result.geometry.disk_size_bytes;
            } else if (argument == L"--soft-limit-bytes") {
                option_seen = &seen.soft;
                destination = &result.geometry.soft_limit_bytes;
            } else if (argument == L"--hard-limit-bytes") {
                option_seen = &seen.hard;
                destination = &result.geometry.hard_limit_bytes;
            } else if (argument == L"--low-watermark-bytes") {
                option_seen = &seen.low;
                destination =
                    &result.geometry.eviction_low_watermark_bytes;
            } else if (argument == L"--max-snapshot-batch-bytes") {
                option_seen = &seen.snapshot_bytes;
                destination = &result.geometry.max_snapshot_batch_bytes;
            } else if (argument == L"--max-snapshot-batch-records") {
                option_seen = &seen.snapshot_records;
                destination = &result.geometry.max_snapshot_batch_records;
            } else {
                option_seen = &seen.stripes;
                destination = &result.geometry.lock_stripe_count;
            }
            parsed = parse_value_option(
                args, index, *option_seen, argument,
                [destination](std::wstring_view value) {
                    auto number = parse_uint64(value);
                    if (!number) {
                        return Result<void>::failure(number.error());
                    }
                    *destination = number.value();
                    return Result<void>::success();
                });
        } else if (argument == L"--test-policy") {
            if (seen.test_policy) return usage_error(L"duplicate --test-policy");
            seen.test_policy = true;
            result.geometry.test_policy_selected = true;
        } else if (argument ==
                   L"--i-understand-this-test-initializes-a-new-disk") {
            if (seen.safety) return usage_error(L"duplicate safety acknowledgement");
            seen.safety = true;
            result.safety_acknowledged = true;
        } else {
            return usage_error(L"unknown argument");
        }
        if (!parsed) return usage_error(parsed.error().message);
    }

    if (!mode) return usage_error(L"an explicit CLI mode is required");
    result.mode = *mode;

    switch (*mode) {
    case M2CliMode::help:
    case M2CliMode::print_schema:
        if (seen.evidence || seen.report || any_real_option(seen)) {
            return usage_error(L"help/schema mode accepts no other options");
        }
        return Result<M2CliConfig>::success(std::move(result));
    case M2CliMode::validate_report:
        if (!seen.report || seen.evidence || any_real_option(seen)) {
            return usage_error(L"validate mode requires only one report path");
        }
        return Result<M2CliConfig>::success(std::move(result));
    case M2CliMode::local_fake:
        if (!seen.evidence || seen.report || any_real_option(seen)) {
            return usage_error(L"local fake mode requires only --evidence-dir");
        }
        return Result<M2CliConfig>::success(std::move(result));
    case M2CliMode::real:
        break;
    }

    if (seen.report || !seen.evidence || !seen.accepted_m1 ||
        !seen.driver_service || !seen.driver_hash || !seen.bus ||
        !seen.vendor || !seen.product || !seen.disk_size ||
        !seen.snapshot_bytes || !seen.snapshot_records || !seen.stripes ||
        !seen.safety) {
        return usage_error(L"real mode is missing a required explicit option");
    }
    if (result.geometry.disk_size_bytes != kRealDiskSizeBytes) {
        return usage_error(L"real mode requires exact 64 GiB disk size");
    }
    const bool any_explicit_limit = seen.soft || seen.hard || seen.low;
    const bool all_explicit_limits = seen.soft && seen.hard && seen.low;
    if ((seen.test_policy && any_explicit_limit) ||
        (!seen.test_policy && !all_explicit_limits)) {
        return usage_error(
            L"select --test-policy or all three explicit limits, never both");
    }

    DynamicBlockStoreOptions options{};
    options.soft_limit_bytes = seen.test_policy
        ? 2 * kBlockSizeBytes
        : result.geometry.soft_limit_bytes;
    options.hard_limit_bytes = seen.test_policy
        ? 3 * kBlockSizeBytes
        : result.geometry.hard_limit_bytes;
    options.eviction_low_watermark_bytes = seen.test_policy
        ? kBlockSizeBytes
        : result.geometry.eviction_low_watermark_bytes;
    options.max_snapshot_batch_bytes =
        result.geometry.max_snapshot_batch_bytes;
    if (result.geometry.max_snapshot_batch_records >
            (std::numeric_limits<std::size_t>::max)() ||
        result.geometry.lock_stripe_count >
            (std::numeric_limits<std::size_t>::max)()) {
        return usage_error(L"store count option does not fit size_t");
    }
    options.max_snapshot_batch_records = static_cast<std::size_t>(
        result.geometry.max_snapshot_batch_records);
    options.lock_stripe_count = static_cast<std::size_t>(
        result.geometry.lock_stripe_count);
    const auto validated = validate_dynamic_block_store_options(options);
    if (!validated.ok()) {
        return usage_error(L"invalid dynamic block store options");
    }

    return Result<M2CliConfig>::success(std::move(result));
}

std::wstring_view m2_cli_usage() noexcept {
    return LR"usage(Usage:
  myrd-m2-e2e --help
  myrd-m2-e2e --print-schema
  myrd-m2-e2e --validate-report <existing-report.json>
  myrd-m2-e2e --local-fake --evidence-dir <existing-directory>
  myrd-m2-e2e --real --evidence-dir <existing-directory>
      --accepted-m1-report <accepted-report.json>
      --expected-driver-service <service> --expected-driver-sha256 <64-hex>
      --expected-bus-type <bus> --expected-vendor-id <vendor>
      --expected-product-id <product> --disk-size-bytes 68719476736
      (--test-policy | --soft-limit-bytes <n> --hard-limit-bytes <n>
       --low-watermark-bytes <n>)
      --max-snapshot-batch-bytes <n> --max-snapshot-batch-records <n>
      --lock-stripe-count <n>
      --i-understand-this-test-initializes-a-new-disk
)usage";
}

} // namespace myrd
