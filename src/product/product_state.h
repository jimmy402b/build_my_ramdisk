#pragma once

#include <cstdint>
#include <string_view>

namespace myrd::product {

enum class RuntimeState : std::uint8_t {
    stopped,
    starting,
    running,
    stopping,
    orphaned,
};

enum class ActivityState : std::uint8_t {
    idle,
    provisioning,
    recovering,
    checkpointing,
    compacting,
    resolving_commit,
};

enum class PersistenceState : std::uint8_t {
    clean,
    dirty,
    persistence_blocked,
    commit_indeterminate,
};

enum class HealthState : std::uint8_t {
    healthy,
    degraded,
    needs_attention,
    fatal,
};

struct DiskState {
    RuntimeState runtime;
    ActivityState activity;
    PersistenceState persistence;
    HealthState health;
};

std::string_view to_string(RuntimeState) noexcept;
std::string_view to_string(ActivityState) noexcept;
std::string_view to_string(PersistenceState) noexcept;
std::string_view to_string(HealthState) noexcept;

bool is_valid(RuntimeState) noexcept;
bool is_valid(ActivityState) noexcept;
bool is_valid(PersistenceState) noexcept;
bool is_valid(HealthState) noexcept;
bool is_valid(const DiskState&) noexcept;

} // namespace myrd::product
