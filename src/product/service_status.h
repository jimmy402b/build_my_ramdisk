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

// **The service's own error codes, which a client has to interpret.**
//
// They travel in a response's `native` field, and a client maps them to an exit code and to a
// sentence.  That makes them part of the public contract rather than an internal detail: a
// client that could not name them would have to hard-code numbers.
enum class ServiceCoreError : std::uint32_t {
    invalid_request = 0x5601,
    unsupported_command,
    authorization_denied,
    profile_not_found,
    state_rejected,
    operation_invalid,
    wait_protocol_invalid,
    dependency_failure,
    // **M5 is a single-disk product, and this code is how that is said.**
    //
    // A second `disk create` / `disk import` or a `disk start` is refused while another
    // disk is up.  It is part of the public contract for the same reason every code here
    // is: a client maps it to an exit code and a sentence, and a client that could not
    // name it would have to hard-code a number.  Appended so every existing code keeps
    // the number already on the wire and in the durable records.
    single_disk_violation,
};
enum class OperationState : std::uint8_t {
    queued = 0,
    running = 1,
    reconciling = 2,
    succeeded = 3,
    failed = 4,
    cancelled = 5,
};

} // namespace myrd::product
