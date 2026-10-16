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

ProductionStartupSweep::ProductionStartupSweep(
    management::IWorkerProcessSource& source,
    const management::IExpectedWorkerBindings& bindings,
    management::WorkerSweepOptions options)
    : source_(source), bindings_(bindings), options_(options) {}

Result<void> ProductionStartupSweep::run_sweep() {
    last_report_.reset();
    last_error_.clear();

    auto report = management::run_worker_sweep(source_, bindings_, options_);
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
