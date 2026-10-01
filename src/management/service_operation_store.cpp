#include "src/management/service_operation_store.h"

#include <chrono>
#include <thread>
#include <vector>

namespace myrd::management {
namespace {

// The wait is polled because the durable operation store has no change
// notification of its own.  The interval only bounds how often the wait
// re-reads durable truth; it never decides what that truth is.
constexpr std::uint64_t kWaitPollIntervalMilliseconds = 50;

} // namespace

void SystemOperationWaitPace::wait_before_repoll() noexcept {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kWaitPollIntervalMilliseconds));
}

ServiceOperationStore::ServiceOperationStore(
    OperationStore store,
    const IServiceClock& clock,
    IOperationWaitPace& pace) noexcept
    : store_(std::move(store)), clock_(&clock), pace_(&pace) {}

Result<ServiceOperationStore> ServiceOperationStore::open_existing(
    const std::wstring_view trusted_program_data_root,
    const IServiceClock& clock,
    IOperationWaitPace& pace) {
    auto store = OperationStore::open_existing(trusted_program_data_root);
    if (!store.ok()) {
        return Result<ServiceOperationStore>::failure(store.error());
    }
    return Result<ServiceOperationStore>::success(
        ServiceOperationStore(store.move_value(), clock, pace));
}

const OperationStorePaths& ServiceOperationStore::paths() const noexcept {
    return store_.paths();
}

Result<std::optional<OperationAdmission>> ServiceOperationStore::find_request(
    const OperationRequest& request) {
    auto found = store_.find_request(request);
    if (!found.ok()) {
        return Result<std::optional<OperationAdmission>>::failure(
            found.error());
    }
    if (!found.value().has_value()) {
        return Result<std::optional<OperationAdmission>>::success(
            std::nullopt);
    }
    // A request that already owns a durable record can only be answered by
    // replaying that record: this lookup never admits anything.
    return Result<std::optional<OperationAdmission>>::success(
        std::optional<OperationAdmission>(OperationAdmission{
            found.move_value().value(), true}));
}

Result<OperationAdmission> ServiceOperationStore::admit(
    const OperationRequest& request,
    const product::Uuid& candidate_operation_id) {
    // The caller mints the operation identifier because it writes the
    // operation gate and the lifecycle reservation under that same identifier
    // before and after this call.  Admission therefore has to honor it.
    return store_.admit_with_id(request, candidate_operation_id);
}

Result<OperationRecord> ServiceOperationStore::load(
    const product::Uuid& operation_id) {
    return store_.load_record(operation_id);
}

Result<void> ServiceOperationStore::write(
    const std::uint64_t expected_revision,
    const OperationRecord& proposed) {
    return store_.write_record(expected_revision, proposed);
}

Result<std::vector<OperationRecord>>
ServiceOperationStore::reconcile_after_restart(
    const std::uint64_t now_unix_ms) {
    return store_.reconcile_after_restart(now_unix_ms);
}

Result<OperationRecord> ServiceOperationStore::request_cancel(
    const product::Uuid& operation_id,
    const std::uint64_t now_unix_ms) {
    return store_.request_cancel(operation_id, now_unix_ms);
}

Result<OperationWaitObservation> ServiceOperationStore::wait_for_change(
    const product::Uuid& operation_id,
    const std::uint64_t after_revision,
    const std::uint64_t monotonic_deadline_ms) {
    for (;;) {
        auto record = store_.load_record(operation_id);
        if (!record.ok()) {
            // A record that is not durable truth is not "no change yet":
            // reporting a timeout for it would let the caller read an absent
            // operation as a slow one.
            return Result<OperationWaitObservation>::failure(record.error());
        }
        if (record.value().revision > after_revision) {
            return Result<OperationWaitObservation>::success(
                OperationWaitObservation{
                    OperationWaitDisposition::changed,
                    record.move_value()});
        }
        if (clock_->monotonic_time_ms() >= monotonic_deadline_ms) {
            // The record is reported even on timeout: the caller learns how
            // far the operation actually got, and the disposition alone
            // decides that the wait, not the operation, ended.
            return Result<OperationWaitObservation>::success(
                OperationWaitObservation{
                    OperationWaitDisposition::timed_out,
                    record.move_value()});
        }
        pace_->wait_before_repoll();
    }
}

} // namespace myrd::management
