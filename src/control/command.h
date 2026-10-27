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
// Writes the disk's durable image to a standalone VHDX at `output`.  Read-only
// with respect to the disk: no profile state, bundle or generation changes.
struct DiskExportCommand {
    std::wstring target;
    std::wstring output;
};
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
    // The volume label written after the volume is mounted (T4-02).  Empty means "leave the default".
    //
    // It is a *create* option and not a profile field only for now: the create path is where a user names a disk,
    // and the label has to be applied to the volume the worker just mounted -- not to a record that may describe a
    // volume which no longer exists.
    std::optional<std::wstring> volume_label;
    // The checkpoint count that makes a compaction due (T4-03).  Declared last so the fields the command already
    // had keep their encoded order; absent means "use the profile's policy default".
    std::optional<std::uint32_t> compaction_checkpoint_threshold;
};

using Command = std::variant<StatusCommand, DiskListCommand, DiskShowCommand,
    DiskStartCommand, DiskCheckpointCommand, DiskCompactCommand,
    DiskConfigGetCommand, OperationListCommand, OperationShowCommand,
    OperationWaitCommand, OperationCancelCommand, DriverStatusCommand,
    DiskConfigSetCommand, DiagnosticsCollectCommand, DiskImportCommand,
    DiskStopCommand, DiskRepairProfileCommand, DiskRebindCommand,
    DiskForgetCommand, DiskCreateCommand, DiskExportCommand>;

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
