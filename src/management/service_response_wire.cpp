// The server-side projection (T3-01).  The wire shape and its codec live in
// `src/client/response_view.cpp`; what is left here needs the durable snapshot, which is a
// service concept, so it stays in the service library that links the client one.

#include "src/management/service_response_wire.h"

#include "src/control/ipc_codec.h"

#include <optional>
#include <string>
#include <utility>

namespace myrd::management {
namespace {

std::optional<std::string> mount_target_of(const profile::Profile& profile) {
    if (profile.mount_target.empty()) return std::nullopt;
    auto converted = ipc::utf16_to_utf8(profile.mount_target);
    // A real Windows path is always valid UTF-16, so this is unreachable for a record
    // this product stored.  Reporting the disk without its mount target is better
    // than failing a list, and far better than printing a lossy replacement for a
    // path that does not exist.
    if (!converted.ok()) return std::nullopt;
    return std::move(converted.value());
}

product::ServiceDiskView view_of(const ServiceProfileProjection& projection) {
    return product::ServiceDiskView{
        projection.profile_id,
        static_cast<product::ServiceLifecycle>(projection.lifecycle),
        static_cast<product::ServiceRuntime>(projection.runtime),
        static_cast<product::ServiceActivity>(projection.activity),
        static_cast<product::ServicePersistence>(projection.persistence),
        static_cast<product::ServiceHealth>(projection.health),
        static_cast<product::ServiceAggregate>(projection.aggregate),
        projection.confirmed_generation,
        projection.mount_target,
        projection.resident_bytes,
        projection.budget_bytes,
        projection.volume_serial,
        projection.successful_compactions,
        projection.checkpoints_since_compaction,
        projection.hard_limit_rejections};
}

product::ServiceOperationView view_of(const ServiceOperationProjection& projection) {
    return product::ServiceOperationView{
        projection.operation_id,
        projection.profile_id,
        static_cast<product::ServiceOperationState>(projection.state),
        projection.phase,
        projection.created_unix_ms,
        projection.updated_unix_ms,
        projection.cancellable,
        projection.cancel_requested};
}

} // namespace
product::ServiceDiskView project_profile(const ManagedProfileSnapshot& snapshot) {
    const auto reported = reported_profile_state(snapshot);
    product::ServiceDiskView projection{
        snapshot.record.profile.profile_uuid,
        static_cast<product::ServiceLifecycle>(snapshot.record.profile.lifecycle),
        static_cast<product::ServiceRuntime>(reported.disk.runtime),
        static_cast<product::ServiceActivity>(reported.disk.activity),
        static_cast<product::ServicePersistence>(reported.disk.persistence),
        static_cast<product::ServiceHealth>(reported.disk.health),
        static_cast<product::ServiceAggregate>(aggregate_status(reported)),
        snapshot.checkpoint.confirmed_generation,
        mount_target_of(snapshot.record.profile)};
    // The memory gauges are a live fact about the runtime, so they are carried only
    // when the read path actually observed one.  A durable snapshot has no opinion
    // about them, and manufacturing one would let a stopped disk report memory.
    if (snapshot.runtime_observation.has_value()) {
        projection.resident_bytes = snapshot.runtime_observation->resident_bytes;
        projection.budget_bytes = snapshot.runtime_observation->budget_bytes;
        projection.volume_serial = snapshot.runtime_observation->volume_serial;
        // R-01: a live fact, like the gauges around it.  A stopped disk reports zero --
        // the count belongs to the runtime that refused the commits.
        projection.hard_limit_rejections =
            snapshot.runtime_observation->hard_limit_rejections;
    }
    // The counters are durable rather than observed, so they are filled whether or not a worker is alive: a
    // stopped disk still knows how many compactions it has completed.
    {
        projection.successful_compactions = snapshot.compaction.successful_compactions_total;
        projection.checkpoints_since_compaction = snapshot.compaction.checkpoints_since_compaction;
    }
    return projection;
}

product::ServiceOperationView project_operation(const OperationRecord& record) {
    return product::ServiceOperationView{
        record.operation_id,
        record.profile_id,
        static_cast<product::ServiceOperationState>(record.state),
        record.phase,
        record.created_unix_ms,
        record.updated_unix_ms,
        record.cancellable,
        record.cancel_requested};
}

ServiceResponseWire project_response(const ServiceResponse& response) {
    client::ServiceResponseWire wire{};
    wire.request_id = response.request_id;
    wire.command = response.command;
    wire.disposition = static_cast<product::ServiceDisposition>(response.disposition);
    wire.operation_id = response.operation_id;
    if (response.operation_projection.has_value()) {
        wire.operation = view_of(response.operation_projection.value());
    }
    if (response.profile_projection.has_value()) {
        wire.profiles.push_back(view_of(response.profile_projection.value()));
    }
    for (const auto& projection : response.profile_projections) {
        wire.profiles.push_back(view_of(projection));
    }
    // Two shapes reach here: an answer built from durable snapshots (what `dispatch`
    // produces) and one that already carries projections.  Deriving from the snapshots when
    // they are present keeps the projection rule in this one file.
    if (response.operation.has_value()) {
        wire.operation = project_operation(response.operation.value());
    }
    if (response.profile.has_value()) {
        wire.profiles.push_back(project_profile(response.profile.value()));
    }
    for (const auto& snapshot : response.profiles) {
        wire.profiles.push_back(project_profile(snapshot));
    }
    wire.warnings = response.warnings;
    wire.error = response.error;
    wire.mount_target = response.mount_target;
    return wire;
}

Result<std::vector<std::byte>> encode_service_response(
    const ServiceResponse& response) {
    return encode_service_response_wire(project_response(response));
}

} // namespace myrd::management