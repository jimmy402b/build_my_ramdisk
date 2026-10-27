#include "src/control/command.h"

#define NOMINMAX
#include <Windows.h>

#include <limits>
#include <type_traits>

namespace myrd::control {

namespace {

template <class>
inline constexpr bool kAlwaysFalse = false;

template <class T>
inline constexpr bool kReadOnlyCommand =
    std::is_same_v<T, StatusCommand>
    || std::is_same_v<T, DiskListCommand>
    || std::is_same_v<T, DiskShowCommand>
    || std::is_same_v<T, DiskConfigGetCommand>
    || std::is_same_v<T, OperationListCommand>
    || std::is_same_v<T, OperationShowCommand>
    || std::is_same_v<T, OperationWaitCommand>
    || std::is_same_v<T, DriverStatusCommand>
    || std::is_same_v<T, DiagnosticsCollectCommand>
    || std::is_same_v<T, DiskExportCommand>;

template <class T>
inline constexpr bool kMutatingCommand =
    std::is_same_v<T, DiskStartCommand>
    || std::is_same_v<T, DiskCheckpointCommand>
    || std::is_same_v<T, DiskCompactCommand>
    || std::is_same_v<T, OperationCancelCommand>
    || std::is_same_v<T, DiskConfigSetCommand>
    || std::is_same_v<T, DiskImportCommand>
    || std::is_same_v<T, DiskStopCommand>
    || std::is_same_v<T, DiskRepairProfileCommand>
    || std::is_same_v<T, DiskRebindCommand>
    || std::is_same_v<T, DiskForgetCommand>
    || std::is_same_v<T, DiskCreateCommand>;

template <class T>
inline constexpr bool kLongOperation =
    std::is_same_v<T, DiskStartCommand>
    || std::is_same_v<T, DiskCheckpointCommand>
    || std::is_same_v<T, DiskCompactCommand>
    || std::is_same_v<T, DiskImportCommand>
    || std::is_same_v<T, DiskStopCommand>
    || std::is_same_v<T, DiskRepairProfileCommand>
    || std::is_same_v<T, DiskRebindCommand>
    || std::is_same_v<T, DiskForgetCommand>
    || std::is_same_v<T, DiskCreateCommand>;

} // namespace

std::string_view command_name(const Command& command) noexcept {
    return std::visit([](const auto& value) -> std::string_view {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, StatusCommand>) {
            return "status";
        } else if constexpr (std::is_same_v<T, DiskListCommand>) {
            return "disk.list";
        } else if constexpr (std::is_same_v<T, DiskShowCommand>) {
            return "disk.show";
        } else if constexpr (std::is_same_v<T, DiskStartCommand>) {
            return "disk.start";
        } else if constexpr (std::is_same_v<T, DiskCheckpointCommand>) {
            return "disk.checkpoint";
        } else if constexpr (std::is_same_v<T, DiskCompactCommand>) {
            return "disk.compact";
        } else if constexpr (std::is_same_v<T, DiskConfigGetCommand>) {
            return "disk.config.get";
        } else if constexpr (std::is_same_v<T, OperationListCommand>) {
            return "operation.list";
        } else if constexpr (std::is_same_v<T, OperationShowCommand>) {
            return "operation.show";
        } else if constexpr (std::is_same_v<T, OperationWaitCommand>) {
            return "operation.wait";
        } else if constexpr (std::is_same_v<T, OperationCancelCommand>) {
            return "operation.cancel";
        } else if constexpr (std::is_same_v<T, DriverStatusCommand>) {
            return "driver.status";
        } else if constexpr (std::is_same_v<T, DiskConfigSetCommand>) {
            return "disk.config.set";
        } else if constexpr (std::is_same_v<T, DiagnosticsCollectCommand>) {
            return "diagnostics.collect";
        } else if constexpr (std::is_same_v<T, DiskImportCommand>) {
            return "disk.import";
        } else if constexpr (std::is_same_v<T, DiskStopCommand>) {
            return "disk.stop";
        } else if constexpr (std::is_same_v<T, DiskRepairProfileCommand>) {
            return "disk.repair-profile";
        } else if constexpr (std::is_same_v<T, DiskRebindCommand>) {
            return "disk.rebind";
        } else if constexpr (std::is_same_v<T, DiskForgetCommand>) {
            return "disk.forget";
        } else if constexpr (std::is_same_v<T, DiskCreateCommand>) {
            return "disk.create";
        } else if constexpr (std::is_same_v<T, DiskExportCommand>) {
            return "disk.export";
        } else {
            static_assert(kAlwaysFalse<T>, "Command name mapping is incomplete");
            return {};
        }
    }, command);
}

bool is_mutating(const Command& command) noexcept {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        static_assert(kReadOnlyCommand<T> || kMutatingCommand<T>,
                      "Command mutation mapping is incomplete");
        return kMutatingCommand<T>;
    }, command);
}

bool is_long_operation(const Command& command) noexcept {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        static_assert(kReadOnlyCommand<T> || kMutatingCommand<T>,
                      "Command duration mapping is incomplete");
        return kLongOperation<T>;
    }, command);
}

std::string_view to_string(IdempotentWarning warning) noexcept {
    switch (warning) {
    case IdempotentWarning::already_running: return "AlreadyRunning";
    case IdempotentWarning::already_stopped: return "AlreadyStopped";
    case IdempotentWarning::no_changes: return "NoChanges";
    }
    return {};
}

bool profile_names_are_unique_case_insensitive(
    std::span<const std::wstring_view> names) noexcept {
    constexpr auto max_length = static_cast<std::size_t>(
        (std::numeric_limits<int>::max)());
    for (std::size_t left = 0; left < names.size(); ++left) {
        if (names[left].size() > max_length) {
            return false;
        }
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (names[right].size() > max_length) {
                return false;
            }
            const int comparison = ::CompareStringOrdinal(
                names[left].data(), static_cast<int>(names[left].size()),
                names[right].data(), static_cast<int>(names[right].size()),
                TRUE);
            if (comparison == 0 || comparison == CSTR_EQUAL) {
                return false;
            }
        }
    }
    return true;
}

} // namespace myrd::control
