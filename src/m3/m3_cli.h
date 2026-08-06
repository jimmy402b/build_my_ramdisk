#pragma once

#include "src/common/result.h"
#include "src/persistence/persistence_types.h"
#include "src/storage/dynamic_block_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace myrd::m3 {

inline constexpr std::uint32_t kM3CliUsageErrorCode = 2;

enum class M3CliCommand : std::uint8_t {
    Help = 0,
    Create,
    Inspect,
    Verify,
    Checkpoint,
    Compact,
    RecoverTest,
    FaultMatrix,
};

[[nodiscard]] constexpr std::string_view command_name(
    M3CliCommand command) noexcept {
    switch (command) {
    case M3CliCommand::Help: return "help";
    case M3CliCommand::Create: return "create";
    case M3CliCommand::Inspect: return "inspect";
    case M3CliCommand::Verify: return "verify";
    case M3CliCommand::Checkpoint: return "checkpoint";
    case M3CliCommand::Compact: return "compact";
    case M3CliCommand::RecoverTest: return "recover-test";
    case M3CliCommand::FaultMatrix: return "fault-matrix";
    }
    return "unknown";
}

[[nodiscard]] Result<M3CliCommand> parse_m3_cli_command(
    std::string_view value);

struct M3CliConfig final {
    M3CliCommand command{M3CliCommand::Help};
    std::filesystem::path bundle_root{};
    std::string manifest_name{};
    std::optional<persistence::ImageGeometry> geometry{};
    persistence::PersistenceLimits persistence_limits{};
    std::optional<DynamicBlockStoreOptions> store_options{};
    std::chrono::milliseconds timeout{0};
    std::optional<persistence::VolumeBarrierKind> barrier{};
    bool full_verify{};
    std::optional<std::string> injected_phase{};
};

// This function performs no filesystem access. In particular, bundle_root is
// retained as supplied and is only checked for safe non-device syntax.
[[nodiscard]] Result<M3CliConfig> parse_m3_cli(
    std::span<const std::wstring_view> args);

[[nodiscard]] std::wstring_view m3_cli_usage() noexcept;

} // namespace myrd::m3
