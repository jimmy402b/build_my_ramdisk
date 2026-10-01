#pragma once

#include "src/management/operation_store.h"
#include "src/management/service_core.h"

#include <cstdint>
#include <string_view>

namespace myrd::management {

// Pacing seam for the bounded wait.  The durable operation store is polled, so
// a wait needs a way to yield between polls.  Production sleeps; tests step
// deterministically so no wall-clock time passes.
class IOperationWaitPace {
public:
    virtual ~IOperationWaitPace() noexcept = default;
    virtual void wait_before_repoll() noexcept = 0;
};

// Real sleep between polls of the bounded operation wait.
class SystemOperationWaitPace final : public IOperationWaitPace {
public:
    void wait_before_repoll() noexcept override;
};

// Production operation authority for the service.  This is a projection of the
// single durable operation store, not a second source of truth: it owns no
// state of its own, and every answer it gives is read from (or written to) the
// same protected records the M4 operation store already persists.  The service
// is the only authority for command, authorization, operation records and
// reconciliation; this adapter only connects it to that store.
class ServiceOperationStore final : public IServiceOperationStore {
public:
    static Result<ServiceOperationStore> open_existing(
        std::wstring_view trusted_program_data_root,
        const IServiceClock& clock,
        IOperationWaitPace& pace);

    // The clock and the pacing step are borrowed for the lifetime of the
    // adapter; taking them by reference keeps either from being null.
    ServiceOperationStore(
        OperationStore store,
        const IServiceClock& clock,
        IOperationWaitPace& pace) noexcept;

    ServiceOperationStore(ServiceOperationStore&&) noexcept = default;
    ServiceOperationStore& operator=(ServiceOperationStore&&) noexcept =
        default;
    ~ServiceOperationStore() noexcept = default;

    ServiceOperationStore(const ServiceOperationStore&) = delete;
    ServiceOperationStore& operator=(const ServiceOperationStore&) = delete;

    Result<std::optional<OperationAdmission>> find_request(
        const OperationRequest& request) override;
    Result<OperationAdmission> admit(
        const OperationRequest& request,
        const product::Uuid& candidate_operation_id) override;
    Result<OperationRecord> load(
        const product::Uuid& operation_id) override;
    Result<void> write(
        std::uint64_t expected_revision,
        const OperationRecord& proposed) override;
    Result<std::vector<OperationRecord>> reconcile_after_restart(
        std::uint64_t now_unix_ms) override;
    Result<OperationRecord> request_cancel(
        const product::Uuid& operation_id,
        std::uint64_t now_unix_ms) override;
    Result<OperationWaitObservation> wait_for_change(
        const product::Uuid& operation_id,
        std::uint64_t after_revision,
        std::uint64_t monotonic_deadline_ms) override;

    const OperationStorePaths& paths() const noexcept;

private:
    OperationStore store_;
    // Neither is owned, and neither is null: the constructor only accepts
    // references.  They are stored as pointers so the adapter stays assignable,
    // which `Result` requires to hand it back from a factory.
    // The wait deadline is expressed on the service clock, so the wait must be
    // measured against that same clock.
    const IServiceClock* clock_{};
    IOperationWaitPace* pace_{};
};

} // namespace myrd::management
