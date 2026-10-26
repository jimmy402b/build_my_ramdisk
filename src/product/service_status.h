#pragma once

// **Two enumerations that were in the management library and belong to the contracts.**
//
// `AggregateStatus` is the single verdict the whole product agrees on -- the service derives
// it, `disk list` prints it, and the GUI renders it -- and `OperationState` is what
// `operation show` reports.  Neither is a service-internal notion, and both were defined in
// headers a client must not include (`state_machine.h`, `operation_store.h`), which is why
// T3-01's boundary could not be drawn without moving them.
//
// The numbers are the ones already stored and already on the wire.  Moving a definition must
// not move a number: `static_assert`s in the contracts' own translation unit pin them, and the
// management library re-exports these names so no call site had to change.

#include <cstdint>
#include <string_view>

namespace myrd::product {

// The verdict a client renders.  Derived by the service through one rule (`aggregate_status`),
// so a client never computes it.
enum class AggregateStatus : std::uint8_t {
    stopped_configured = 0,
    running,
    blocked_needs_attention,
    failed,
};

inline std::string_view to_string(AggregateStatus status) noexcept {
    switch (status) {
    case AggregateStatus::stopped_configured: return "stopped/configured";
    case AggregateStatus::running: return "running";
    case AggregateStatus::blocked_needs_attention: return "blocked/needs-attention";
    case AggregateStatus::failed: return "failed";
    }
    // Unreachable for a valid enumerator; "failed" is the fail-closed answer if it ever is
    // reached, because a verdict that cannot be named must not read as a good one.
    return "failed";
}

enum class OperationState : std::uint8_t {
    queued = 0,
    running = 1,
    reconciling = 2,
    succeeded = 3,
    failed = 4,
    cancelled = 5,
};

} // namespace myrd::product
