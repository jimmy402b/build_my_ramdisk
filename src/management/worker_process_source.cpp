#include "src/management/worker_process_source.h"

#include <algorithm>

namespace myrd::management {
namespace {

const ExpectedWorkerBinding* find_binding(
    const std::vector<ExpectedWorkerBinding>& expected,
    const product::Uuid& profile_id) noexcept {
    const auto found = std::find_if(
        expected.begin(), expected.end(),
        [&profile_id](const ExpectedWorkerBinding& binding) {
            return binding.profile_id == profile_id;
        });
    return found == expected.end() ? nullptr : &*found;
}

} // namespace

std::vector<SweepVerdictEntry> sweep_verdicts(
    const std::vector<OrphanedProcess>& observed,
    const std::vector<ExpectedWorkerBinding>& expected) {
    std::vector<SweepVerdictEntry> verdicts;
    verdicts.reserve(observed.size());

    for (const auto& process : observed) {
        SweepVerdictEntry entry{};
        entry.process_id = process.process_id;
        entry.profile_id = process.profile_id;

        if (process.process_id == 0) {
            // Not a process.  A job enumeration that produced this cannot be
            // trusted to have produced the rest of the list either, so nothing is
            // acted on.
            entry.verdict = SweepVerdict::leave_alone;
            verdicts.push_back(entry);
            continue;
        }

        if (!process.profile_id.has_value()) {
            // Inside our job, so this service started it; no binding, so no
            // operation can ever address it again.  Forced, because there is no
            // profile whose ordered teardown it could run.
            entry.verdict = SweepVerdict::terminate_forced;
            verdicts.push_back(entry);
            continue;
        }

        const ExpectedWorkerBinding* binding =
            find_binding(expected, *process.profile_id);
        if (binding == nullptr) {
            // A binding we do not have a record for.  Treated exactly like no
            // binding: the service cannot say which disk this process served, and
            // a worker it cannot place is one it cannot adopt or wait for.
            entry.verdict = SweepVerdict::terminate_forced;
            verdicts.push_back(entry);
            continue;
        }

        // The load-bearing comparison, and the reason this is not decided by the
        // job alone.  Windows recycles pids, so a pid that matches while the
        // creation time does not is a *different process* wearing a dead worker's
        // number.  Terminating it kills a stranger; adopting it trusts one.
        if (!binding->recorded_creation_time.has_value()
            || !process.process_creation_time.has_value()) {
            entry.verdict = SweepVerdict::leave_alone;
            verdicts.push_back(entry);
            continue;
        }
        if (*process.process_creation_time
            != *binding->recorded_creation_time) {
            entry.verdict = SweepVerdict::leave_alone;
            verdicts.push_back(entry);
            continue;
        }

        // The process the binding recorded, and its profile's disk is expected to
        // be serving.  Adopting is the only honest answer: terminating would stop
        // a disk the operator believes is running, and the profile's own
        // start/stop path is what owns that decision.
        entry.verdict = binding->expects_running_worker
            ? SweepVerdict::adopt
            : SweepVerdict::terminate_cooperatively;
        verdicts.push_back(entry);
    }

    return verdicts;
}

} // namespace myrd::management
