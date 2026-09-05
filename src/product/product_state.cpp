#include "src/product/product_state.h"

namespace myrd::product {

std::string_view to_string(RuntimeState s) noexcept {
    switch (s) {
    case RuntimeState::stopped:  return "Stopped";
    case RuntimeState::starting: return "Starting";
    case RuntimeState::running:  return "Running";
    case RuntimeState::stopping: return "Stopping";
    case RuntimeState::orphaned: return "Orphaned";
    }
    return {};
}

bool is_valid(RuntimeState s) noexcept {
    switch (s) {
    case RuntimeState::stopped:
    case RuntimeState::starting:
    case RuntimeState::running:
    case RuntimeState::stopping:
    case RuntimeState::orphaned:
        return true;
    }
    return false;
}

std::string_view to_string(ActivityState s) noexcept {
    switch (s) {
    case ActivityState::idle:              return "Idle";
    case ActivityState::provisioning:      return "Provisioning";
    case ActivityState::recovering:        return "Recovering";
    case ActivityState::checkpointing:     return "Checkpointing";
    case ActivityState::compacting:        return "Compacting";
    case ActivityState::resolving_commit:  return "ResolvingCommit";
    }
    return {};
}

bool is_valid(ActivityState s) noexcept {
    switch (s) {
    case ActivityState::idle:
    case ActivityState::provisioning:
    case ActivityState::recovering:
    case ActivityState::checkpointing:
    case ActivityState::compacting:
    case ActivityState::resolving_commit:
        return true;
    }
    return false;
}

std::string_view to_string(PersistenceState s) noexcept {
    switch (s) {
    case PersistenceState::clean:                return "Clean";
    case PersistenceState::dirty:                return "Dirty";
    case PersistenceState::persistence_blocked:  return "PersistenceBlocked";
    case PersistenceState::commit_indeterminate: return "CommitIndeterminate";
    }
    return {};
}

bool is_valid(PersistenceState s) noexcept {
    switch (s) {
    case PersistenceState::clean:
    case PersistenceState::dirty:
    case PersistenceState::persistence_blocked:
    case PersistenceState::commit_indeterminate:
        return true;
    }
    return false;
}

std::string_view to_string(HealthState s) noexcept {
    switch (s) {
    case HealthState::healthy:         return "Healthy";
    case HealthState::degraded:        return "Degraded";
    case HealthState::needs_attention: return "NeedsAttention";
    case HealthState::fatal:           return "Fatal";
    }
    return {};
}

bool is_valid(HealthState s) noexcept {
    switch (s) {
    case HealthState::healthy:
    case HealthState::degraded:
    case HealthState::needs_attention:
    case HealthState::fatal:
        return true;
    }
    return false;
}

bool is_valid(const DiskState& ds) noexcept {
    return is_valid(ds.runtime) && is_valid(ds.activity)
        && is_valid(ds.persistence) && is_valid(ds.health);
}

} // namespace myrd::product
