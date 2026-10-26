#pragma once

// **What a client is allowed to know, in a vocabulary that is not the service's.**
//
// The service's own answer holds `ManagedProfileSnapshot` values: bundle paths, ownership,
// journals, operation gates.  The projection that strips those down lives in the management
// library, and it speaks in `product::RuntimeState`, `AggregateStatus`, `OperationState` --
// service concepts, fine inside the service and unnecessary in a client that only renders
// a list.
//
// T3-01's boundary needs the client to reach none of that.  So this header defines the
// *neutral* vocabulary: small enumerations and plain structs, owned by the contracts
// library, that both sides can speak:
//
//   service   durable snapshot  ->  neutral view  ->  wire bytes
//                                     ^
//   client                          wire bytes  ->  neutral view
//
// The mapping from the service's concepts to these is explicit and exhaustive (a switch, so
// a new runtime state cannot be forgotten), and it is the one place the two vocabularies are
// allowed to meet.  A client that renders `aggregate` renders the service's verdict, not one
// it computed itself.
//
// These are wire-level values: the integers are the ones on the wire, so an added enumerator
// is a format change and has to be a deliberate one.

#include "src/product/uuid.h"

#include <cstdint>
#include <optional>
#include <string>

namespace myrd::product {

// Mirrors the service's lifecycle, runtime, activity, persistence and health states.  The
// numbers are the wire's; `unknown` exists so that a byte this build does not understand
// decodes to something visibly unknown instead of to a plausible-looking default.
enum class ServiceLifecycle : std::uint8_t {
    unknown = 0,
    provisioning = 1,
    ready = 2,
    failed = 3,
};

enum class ServiceRuntime : std::uint8_t {
    unknown = 0,
    stopped = 1,
    starting = 2,
    running = 3,
    stopping = 4,
    orphaned = 5,
};

enum class ServiceActivity : std::uint8_t {
    unknown = 0,
    idle = 1,
    provisioning = 2,
    recovering = 3,
    checkpointing = 4,
    compacting = 5,
    resolving_commit = 6,
};

enum class ServicePersistence : std::uint8_t {
    unknown = 0,
    clean = 1,
    dirty = 2,
    persistence_blocked = 3,
    commit_indeterminate = 4,
};

enum class ServiceHealth : std::uint8_t {
    unknown = 0,
    healthy = 1,
    degraded = 2,
    needs_attention = 3,
    fatal = 4,
};

// The single verdict a client renders.  The service derives it through the same rule its
// own read path uses, so a client cannot disagree with the service about whether a disk is
// fine.  Four values, matching `AggregateStatus` exactly: a client that invents a fifth
// verdict is a client rendering something the service never said.
enum class ServiceAggregate : std::uint8_t {
    unknown = 0,
    stopped_configured = 1,
    running = 2,
    blocked_needs_attention = 3,
    failed = 4,
};

enum class ServiceOperationState : std::uint8_t {
    unknown = 0,
    queued = 1,
    running = 2,
    reconciling = 3,
    succeeded = 4,
    failed = 5,
    cancelled = 6,
};

enum class ServiceDisposition : std::uint8_t {
    unknown = 0,
    completed = 1,
    operation_queued = 2,
    dry_run = 3,
    wait_timeout = 4,
    client_wait_cancelled = 5,
};

// One disk, as a client sees it.  No bundle path, no journal, no owner, no host address --
// the fields the design's section 10.3 names, plus the mount target, which is what a user
// recognises a disk by.
struct ServiceDiskView final {
    Uuid profile_id;
    ServiceLifecycle lifecycle{ServiceLifecycle::unknown};
    ServiceRuntime runtime{ServiceRuntime::unknown};
    ServiceActivity activity{ServiceActivity::unknown};
    ServicePersistence persistence{ServicePersistence::unknown};
    ServiceHealth health{ServiceHealth::unknown};
    ServiceAggregate aggregate{ServiceAggregate::unknown};
    std::uint64_t confirmed_generation{};
    std::optional<std::string> mount_target;
    // The two numbers the CLI and the GUI show, and the reason they are here: "memory is
    // under control" is only checkable if a client can see the disk's resident bytes against
    // its budget.  A stopped runtime reports zero for both, which is the truth.
    std::uint64_t resident_bytes{};
    std::uint64_t budget_bytes{};
    bool operator==(const ServiceDiskView&) const noexcept = default;
};

struct ServiceOperationView final {
    Uuid operation_id;
    Uuid profile_id;
    ServiceOperationState state{ServiceOperationState::unknown};
    std::string phase;
    std::uint64_t created_unix_ms{};
    std::uint64_t updated_unix_ms{};
    bool cancellable{};
    bool cancel_requested{};
    bool operator==(const ServiceOperationView&) const noexcept = default;
};

// True when the value is one this build knows.  A decoder uses it to refuse an enumerator
// from a newer document rather than showing it as `unknown`.
constexpr bool known_lifecycle(const ServiceLifecycle value) noexcept {
    return value >= ServiceLifecycle::provisioning && value <= ServiceLifecycle::failed;
}
constexpr bool known_runtime(const ServiceRuntime value) noexcept {
    return value >= ServiceRuntime::stopped && value <= ServiceRuntime::orphaned;
}
constexpr bool known_activity(const ServiceActivity value) noexcept {
    return value >= ServiceActivity::idle && value <= ServiceActivity::resolving_commit;
}
constexpr bool known_persistence(const ServicePersistence value) noexcept {
    return value >= ServicePersistence::clean
        && value <= ServicePersistence::commit_indeterminate;
}
constexpr bool known_health(const ServiceHealth value) noexcept {
    return value >= ServiceHealth::healthy && value <= ServiceHealth::fatal;
}
constexpr bool known_aggregate(const ServiceAggregate value) noexcept {
    return value >= ServiceAggregate::stopped_configured
        && value <= ServiceAggregate::failed;
}
constexpr bool known_operation_state(const ServiceOperationState value) noexcept {
    return value >= ServiceOperationState::queued
        && value <= ServiceOperationState::cancelled;
}
constexpr bool known_disposition(const ServiceDisposition value) noexcept {
    return value >= ServiceDisposition::completed
        && value <= ServiceDisposition::client_wait_cancelled;
}

} // namespace myrd::product
