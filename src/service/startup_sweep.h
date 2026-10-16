#pragma once

// The bridge that lets the sweep act: durable fingerprints become expected
// bindings, and the production sweep becomes an `IStartupSweep`.
//
// Two adapters, together, because either alone is useless.  `WorkerBindingStore`
// holds what the *previous* service session observed; `sweep_verdicts` needs that
// as `ExpectedWorkerBinding`s or every orphan it finds is unattributable and the
// sweep can only report them.  And `run_worker_sweep` returns a report, while the
// host wants an `IStartupSweep` it can call once and ask a yes/no question of.
//
// **The creation time must come from the store, and that is the whole point.**
// Without it, `sweep_verdicts` answers `leave_alone` for every orphan -- correctly,
// since a pid alone is not an identity -- and a sweep that silently never acts is
// the failure mode this file exists to prevent.  It is stated here because it is
// invisible in the types: an empty `recorded_creation_time` is legal and produces a
// sweep that always declines.

#include "src/common/result.h"
#include "src/management/worker_binding_store.h"
#include "src/management/worker_sweep.h"
#include "src/management/worker_sweep_policy.h"
#include "src/product/uuid.h"
#include "src/service/service_host.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace myrd::service {

// The durable fingerprints, as the bindings the sweep judges against.
//
// `expects_running_worker` is taken from a caller-supplied predicate rather than
// guessed from the fingerprint, because "should this profile's disk be serving?"
// is a question about the durable *profile* state, which this adapter does not
// read.  A fingerprint records which process a profile last had; it says nothing
// about whether that profile is supposed to be up.
class FingerprintWorkerBindings final : public management::IExpectedWorkerBindings {
public:
    // Whether the profile's disk is expected to be running right now.  Called once
    // per recorded fingerprint; absent means "not expected running", which is the
    // conservative answer -- terminating a worker whose disk should have been
    // serving is worse than terminating one that should not, because the first
    // stops something the operator believes is up.
    using RunningPredicate =
        std::function<bool(const product::Uuid&)>;

    FingerprintWorkerBindings(const management::WorkerBindingStore& store,
                              RunningPredicate is_expected_running);

    [[nodiscard]] Result<std::vector<management::ExpectedWorkerBinding>> expected()
        const override;

private:
    const management::WorkerBindingStore& store_;
    RunningPredicate is_expected_running_;
};

// The sweep, as the host calls it.
class ProductionStartupSweep final : public IStartupSweep {
public:
    ProductionStartupSweep(management::IWorkerProcessSource& source,
                           const management::IExpectedWorkerBindings& bindings,
                           management::WorkerSweepOptions options = {});

    [[nodiscard]] Result<void> run_sweep() override;
    [[nodiscard]] bool needs_attention() const noexcept override;

    // The last report, for the operator and for evidence.  Absent until a sweep
    // has run, so "not swept yet" is not confused with "swept and found nothing".
    [[nodiscard]] const std::optional<management::WorkerSweepReport>& last_report()
        const noexcept {
        return last_report_;
    }

    [[nodiscard]] const std::wstring& last_error() const noexcept {
        return last_error_;
    }

private:
    management::IWorkerProcessSource& source_;
    const management::IExpectedWorkerBindings& bindings_;
    management::WorkerSweepOptions options_{};
    std::optional<management::WorkerSweepReport> last_report_;
    std::wstring last_error_;
};

} // namespace myrd::service
