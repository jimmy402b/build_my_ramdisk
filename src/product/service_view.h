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
#include <string_view>
#include <optional>
#include <string>

namespace myrd::product {

// Mirrors the service's lifecycle, runtime, activity, persistence and health states, with the
// same numbers they have on the wire.
// **These are the wire's numbers, and they are the service's own numbers.**  An earlier draft
// offset every value by one to reserve 0 for "unknown"; that would have changed the format to
// buy a state the strict decoder never produces (it rejects unknown enumerators rather than
// inventing one).  Identical values mean the split can happen without touching a single byte
// on the wire, which is the only way it can happen safely.
// Two values, because a profile's durable lifecycle has two: it is provisioning until the disk
// exists, and ready afterwards.  A third value here would be a client state the service can
// never produce.
enum class ServiceLifecycle : std::uint8_t {
    provisioning = 0,
    ready,
};

enum class ServiceRuntime : std::uint8_t {
    stopped = 0,
    starting,
    running,
    stopping,
    orphaned,
};

enum class ServiceActivity : std::uint8_t {
    idle = 0,
    provisioning,
    recovering,
    checkpointing,
    compacting,
    resolving_commit,
};

enum class ServicePersistence : std::uint8_t {
    clean = 0,
    dirty,
    persistence_blocked,
    commit_indeterminate,
};

enum class ServiceHealth : std::uint8_t {
    healthy = 0,
    degraded,
    needs_attention,
    fatal,
};

// The single verdict a client renders.  The service derives it through the same rule its
// own read path uses, so a client cannot disagree with the service about whether a disk is
// fine.  Four values, matching `AggregateStatus` exactly: a client that invents a fifth
// verdict is a client rendering something the service never said.
enum class ServiceAggregate : std::uint8_t {
    stopped_configured = 0,
    running,
    blocked_needs_attention,
    failed,
};

enum class ServiceOperationState : std::uint8_t {
    queued = 0,
    running,
    reconciling,
    succeeded,
    failed,
    cancelled,
};

enum class ServiceDisposition : std::uint8_t {
    completed = 0,
    operation_queued,
    dry_run,
    wait_timeout,
    client_wait_cancelled,
};

// The spellings a client shows.  Identical to the service's own (`product::to_string` over its
// states), because a client that spells a state differently from the service is two products
// wearing one name -- and the CLI's output is what the tests read.
inline std::string_view to_string(ServiceAggregate value) noexcept {
    switch (value) {
    case ServiceAggregate::stopped_configured: return "stopped/configured";
    case ServiceAggregate::running: return "running";
    case ServiceAggregate::blocked_needs_attention: return "blocked/needs-attention";
    case ServiceAggregate::failed: return "failed";
    }
    return "failed";
}
inline std::string_view to_string(ServiceRuntime value) noexcept {
    switch (value) {
    case ServiceRuntime::stopped: return "Stopped";
    case ServiceRuntime::starting: return "Starting";
    case ServiceRuntime::running: return "Running";
    case ServiceRuntime::stopping: return "Stopping";
    case ServiceRuntime::orphaned: return "Orphaned";
    }
    return "Stopped";
}

inline std::string_view to_string(ServiceActivity value) noexcept {
    switch (value) {
    case ServiceActivity::idle: return "Idle";
    case ServiceActivity::provisioning: return "Provisioning";
    case ServiceActivity::recovering: return "Recovering";
    case ServiceActivity::checkpointing: return "Checkpointing";
    case ServiceActivity::compacting: return "Compacting";
    case ServiceActivity::resolving_commit: return "ResolvingCommit";
    }
    return "Idle";
}

inline std::string_view to_string(ServicePersistence value) noexcept {
    switch (value) {
    case ServicePersistence::clean: return "Clean";
    case ServicePersistence::dirty: return "Dirty";
    case ServicePersistence::persistence_blocked: return "PersistenceBlocked";
    case ServicePersistence::commit_indeterminate: return "CommitIndeterminate";
    }
    return "PersistenceBlocked";
}

inline std::string_view to_string(ServiceHealth value) noexcept {
    switch (value) {
    case ServiceHealth::healthy: return "Healthy";
    case ServiceHealth::degraded: return "Degraded";
    case ServiceHealth::needs_attention: return "NeedsAttention";
    case ServiceHealth::fatal: return "Fatal";
    }
    return "NeedsAttention";
}

inline std::string_view to_string(ServiceLifecycle value) noexcept {
    switch (value) {
    case ServiceLifecycle::provisioning: return "Provisioning";
    case ServiceLifecycle::ready: return "Ready";
    }
    return "Provisioning";
}

inline std::string_view to_string(ServiceOperationState value) noexcept {
    switch (value) {
    case ServiceOperationState::queued: return "queued";
    case ServiceOperationState::running: return "running";
    case ServiceOperationState::reconciling: return "reconciling";
    case ServiceOperationState::succeeded: return "succeeded";
    case ServiceOperationState::failed: return "failed";
    case ServiceOperationState::cancelled: return "cancelled";
    }
    return "failed";
}

inline std::string_view to_string(ServiceDisposition value) noexcept {
    switch (value) {
    case ServiceDisposition::completed: return "completed";
    case ServiceDisposition::operation_queued: return "operation queued";
    case ServiceDisposition::dry_run: return "dry run";
    case ServiceDisposition::wait_timeout: return "wait timed out";
    case ServiceDisposition::client_wait_cancelled: return "client wait cancelled";
    }
    return "completed";
}
// One disk, as a client sees it.  No bundle path, no journal, no owner, no host address --
// the fields the design's section 10.3 names, plus the mount target, which is what a user
// recognises a disk by.
struct ServiceDiskView final {
    Uuid profile_id;
    ServiceLifecycle lifecycle{ServiceLifecycle::provisioning};
    ServiceRuntime runtime{ServiceRuntime::stopped};
    ServiceActivity activity{ServiceActivity::idle};
    ServicePersistence persistence{ServicePersistence::clean};
    ServiceHealth health{ServiceHealth::healthy};
    ServiceAggregate aggregate{ServiceAggregate::stopped_configured};
    std::uint64_t confirmed_generation{};
    std::optional<std::string> mount_target;
    // The two numbers the CLI and the GUI show, and the reason they are here: "memory is
    // under control" is only checkable if a client can see the disk's resident bytes against
    // its budget.  A stopped runtime reports zero for both, which is the truth.
    std::uint64_t resident_bytes{};
    std::uint64_t budget_bytes{};
    std::uint64_t volume_serial{};
    // **Compaction, made visible (T4-03).**  The policy has bounded journal growth since the schema was written and
    // nothing in the service ever read it; the counters are durable, the log now reports them, and these two are what
    // a client needs to see the same thing happen.  Zero means no compaction has completed, which is the truth for a
    // disk that has never needed one.
    std::uint64_t successful_compactions{};
    std::uint64_t checkpoints_since_compaction{};
    bool operator==(const ServiceDiskView&) const noexcept = default;
};

struct ServiceOperationView final {
    Uuid operation_id;
    Uuid profile_id;
    ServiceOperationState state{ServiceOperationState::queued};
    std::string phase;
    std::uint64_t created_unix_ms{};
    std::uint64_t updated_unix_ms{};
    bool cancellable{};
    bool cancel_requested{};
    bool operator==(const ServiceOperationView&) const noexcept = default;
};

// The decoder rejects an enumerator outside its known range, so there is no `known_*` helper
// here: a value that reaches these types is one this build understands.

} // namespace myrd::product
