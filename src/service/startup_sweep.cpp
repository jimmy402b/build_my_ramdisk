#include "src/service/startup_sweep.h"

#include <utility>

namespace myrd::service {

FingerprintWorkerBindings::FingerprintWorkerBindings(
    const management::WorkerBindingStore& store,
    RunningPredicate is_expected_running)
    : store_(store), is_expected_running_(std::move(is_expected_running)) {}

Result<std::vector<management::ExpectedWorkerBinding>>
FingerprintWorkerBindings::expected() const {
    auto records = store_.list();
    if (!records.ok()) {
        // Propagated rather than answered with an empty list.  An empty list means
        // "no profile is expected to have a worker", which makes every orphan
        // foreign and terminates it -- so a store read failure would be turned
        // into a mass termination.  `run_worker_sweep` treats a failure here as
        // fatal for exactly this reason, and this is where that failure is made.
        return Result<std::vector<management::ExpectedWorkerBinding>>::failure(
            records.error());
    }

    std::vector<management::ExpectedWorkerBinding> bindings;
    bindings.reserve(records.value().size());
    for (const auto& record : records.value()) {
        management::ExpectedWorkerBinding binding{
            record.profile_id,
            record.worker_process_creation_time,
            is_expected_running_ != nullptr
                && is_expected_running_(record.profile_id)};
        bindings.push_back(std::move(binding));
    }
    return Result<std::vector<management::ExpectedWorkerBinding>>::success(
        std::move(bindings));
}

ProfileResourceIdentities::ProfileResourceIdentities(
    management::IServiceProfileStore& store) noexcept
    : store_(store) {}

Result<std::vector<std::pair<product::Uuid,
                             std::optional<management::ObservedResourceIdentity>>>>
ProfileResourceIdentities::observed_identities() const {
    auto snapshots = store_.list_profiles();
    if (!snapshots.ok()) {
        // Propagated, never answered with an empty list: an empty list means "no
        // profile left anything behind", which would let a read failure look like a
        // clean machine.
        return Result<std::vector<std::pair<
            product::Uuid, std::optional<management::ObservedResourceIdentity>>>>::
            failure(snapshots.error());
    }

    std::vector<std::pair<product::Uuid,
                          std::optional<management::ObservedResourceIdentity>>>
        identities;
    identities.reserve(snapshots.value().size());
    for (const auto& snapshot : snapshots.value()) {
        // The generation the checkpoint state stands at, and the bundle file identity
        // that is deliberately absent -- see the header for why, and for why absent
        // here means "unprovable" rather than "nothing to do".
        auto identity = management::project_observed_identity(
            snapshot.record, std::nullopt,
            snapshot.checkpoint.confirmed_generation);
        identities.emplace_back(snapshot.record.profile.profile_uuid,
                                std::move(identity));
    }
    return Result<std::vector<std::pair<
        product::Uuid, std::optional<management::ObservedResourceIdentity>>>>::
        success(std::move(identities));
}

ProductionStartupSweep::ProductionStartupSweep(
    management::IWorkerProcessSource& source,
    const management::IExpectedWorkerBindings& bindings,
    management::WorkerSweepOptions options,
    const management::IObservedResourceIdentitySource* const identities,
    management::IResourceReclaimer* const reclaimer)
    : source_(source), bindings_(bindings), options_(options),
      identities_(identities), reclaimer_(reclaimer) {}

Result<void> ProductionStartupSweep::run_sweep() {
    last_report_.reset();
    last_error_.clear();

    auto report = management::run_worker_sweep(source_, bindings_, options_,
                                              identities_, reclaimer_);
    if (!report.ok()) {
        last_error_ = report.error().message;
        return Result<void>::failure(report.error());
    }
    last_report_ = report.move_value();
    if (last_report_->needs_attention()) {
        // Kept as text as well as in the report, because this is what ends up in
        // the log and an operator reads the log after a reboot.
        last_error_ = last_report_->first_failure;
    }
    return Result<void>::success();
}

bool ProductionStartupSweep::needs_attention() const noexcept {
    return last_report_.has_value() && last_report_->needs_attention();
}

} // namespace myrd::service
