#pragma once

#include "src/product/uuid.h"
#include "src/product/quantity_parser.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <vector>

namespace myrd::control {

struct DiskShowCommand {
    std::wstring target;
};

struct DiskStartCommand {
    std::wstring target;
};

struct StatusCommand {};
struct DiskListCommand {};
struct DiskCheckpointCommand { std::wstring target; };
struct DiskCompactCommand { std::wstring target; };
struct DiskConfigGetCommand { std::wstring target; };
struct OperationListCommand {};
struct OperationShowCommand { product::Uuid operation_id; };
struct OperationWaitCommand { product::Uuid operation_id; };
struct OperationCancelCommand { product::Uuid operation_id; };
struct DriverStatusCommand {};
struct ConfigAssignment { std::wstring key; std::wstring value; };
struct DiskConfigSetCommand {
    std::wstring target;
    std::vector<ConfigAssignment> assignments;
};
struct DiagnosticsCollectCommand {
    std::optional<std::wstring> disk;
    std::optional<std::wstring> output;
    bool include_paths{};
};
struct DiskImportCommand {
    std::wstring name;
    std::wstring bundle;
    std::wstring mount;
    std::optional<std::wstring> owner_sid;
};
struct DiskStopCommand {
    std::wstring target;
    bool force{};
    bool accept_volatile_data_loss{};
};
struct DiskRepairProfileCommand { product::Uuid profile_id; bool from_previous{}; };
struct DiskRebindCommand {
    product::Uuid profile_id;
    std::wstring bundle;
    product::Uuid expected_image_id;
};
struct DiskForgetCommand {
    product::Uuid profile_id;
    product::Uuid confirmation_id;
};
enum class MemoryMode : std::uint8_t { automatic, fixed };
struct OptionalSize {
    bool enabled;
    std::uint64_t bytes;
};
struct DiskCreateCommand {
    std::wstring name;
    std::wstring bundle;
    std::uint64_t size_bytes;
    std::wstring mount;
    std::optional<std::wstring> owner_sid;
    bool no_start{};
    std::optional<MemoryMode> memory_mode;
    std::optional<std::uint64_t> memory_hard_limit_bytes;
    std::optional<product::CheckpointInterval> checkpoint_interval;
    std::optional<OptionalSize> dirty_threshold;
};

using Command = std::variant<StatusCommand, DiskListCommand, DiskShowCommand,
    DiskStartCommand, DiskCheckpointCommand, DiskCompactCommand,
    DiskConfigGetCommand, OperationListCommand, OperationShowCommand,
    OperationWaitCommand, OperationCancelCommand, DriverStatusCommand,
    DiskConfigSetCommand, DiagnosticsCollectCommand, DiskImportCommand,
    DiskStopCommand, DiskRepairProfileCommand, DiskRebindCommand,
    DiskForgetCommand, DiskCreateCommand>;

std::string_view command_name(const Command& command) noexcept;
bool is_mutating(const Command& command) noexcept;
bool is_long_operation(const Command& command) noexcept;

enum class IdempotentWarning : std::uint8_t {
    already_running,
    already_stopped,
    no_changes,
};

std::string_view to_string(IdempotentWarning warning) noexcept;
bool profile_names_are_unique_case_insensitive(
    std::span<const std::wstring_view> names) noexcept;

} // namespace myrd::control
