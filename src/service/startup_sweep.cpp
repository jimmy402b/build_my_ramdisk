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

LeftoverReclaimer::LeftoverReclaimer(
    management::IServiceProfileStore& profiles,
    management::IWorkerControl& worker) noexcept
    : profiles_(profiles), worker_(worker) {}

Result<bool> LeftoverReclaimer::reclaim(
    const product::Uuid& profile_id,
    const management::ObservedResourceIdentity& identity) {
    last_failure_.clear();

    // The mount the identity names is the *proof* that this leftover is the profile's; the
    // launch facts come from the record.  A profile with no letter in its record cannot be
    // dismantled by this path, because the worker finds the volume by that letter -- so
    // refusing here is honest rather than a shortcut.
    if (identity.mount_target == 0) {
        last_failure_ = L"the reclaimed identity names no drive letter, so the leftover "
                        L"cannot be located without guessing";
        return Result<bool>::success(false);
    }

    auto snapshot = profiles_.load(profile_id);
    if (!snapshot.ok()) {
        // Re-reading failed, so what the profile *is now* is unknown.  Refused rather than
        // acting on the identity's older view: dismantling a volume on a stale reading is
        // how a reclaim removes something that has since become a different disk.
        last_failure_ = snapshot.error().message;
        return Result<bool>::success(false);
    }

    // `product::Uuid` has no default constructor, so the request is built as an aggregate
    // with the three ids it carries -- all the same id here, because a reclaim is
    // attributed to the profile it dismantles and inventing an operation no durable record
    // knows about would make the teardown unattributable.
    management::WorkerActionRequest request{
        profile_id,
        profile_id,
        profile_id,
        management::ManagementCommand::stop,
        management::ActionKind::reclaim_leftover,
        std::nullopt,
        snapshot.value().record,
        std::nullopt};

    const auto reclaimed = worker_.reclaim_leftover(request);
    if (!reclaimed.ok()) {
        last_failure_ = reclaimed.error().message;
        return Result<bool>::success(false);
    }
    // A teardown that reported success while the letter survived would be exactly the
    // "success while doing nothing" outcome this increment has met four times, so the
    // worker's own answer is what is reported rather than the call having returned.
    return Result<bool>::success(true);
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
