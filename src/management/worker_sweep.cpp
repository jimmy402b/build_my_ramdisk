#include "src/management/worker_sweep.h"

#include <utility>

namespace myrd::management {

Result<WorkerSweepReport> run_worker_sweep(
    IWorkerProcessSource& source, const IExpectedWorkerBindings& bindings,
    const WorkerSweepOptions& options) {
    // What the job holds that this session did not launch.  Read before anything
    // is touched, so the report describes the world the sweep found rather than
    // the one it made.
    auto observed = source.observe_orphans();
    if (!observed.ok()) {
        // The only genuine failure of the sweep as a whole: with no enumeration
        // there is nothing to decide about, and reporting "nothing to do" would be
        // the one answer that is definitely wrong.
        return Result<WorkerSweepReport>::failure(observed.error());
    }

    auto expected = bindings.expected();
    if (!expected.ok()) {
        // Also fatal, and deliberately: without the durable bindings every orphan
        // would look like "no binding we recognise" and be forced down, so a
        // store that failed to open would silently turn into a mass termination of
        // processes that may well have been ours, mid-teardown.
        return Result<WorkerSweepReport>::failure(expected.error());
    }

    const auto verdicts = sweep_verdicts(observed.value(), expected.value());

    WorkerSweepReport report{};
    report.orphans_observed =
        static_cast<std::uint32_t>(observed.value().size());

    for (const auto& verdict : verdicts) {
        // The process this verdict is about, by pid.  Found rather than paired by
        // index on purpose: `sweep_verdicts` preserves order today, but a caller
        // that depended on that would break silently the first time it did not.
        const OrphanedProcess* process = nullptr;
        for (const auto& candidate : observed.value()) {
            if (candidate.process_id == verdict.process_id) {
                process = &candidate;
                break;
            }
        }
        if (process == nullptr) continue;

        switch (verdict.verdict) {
        case SweepVerdict::adopt:
            ++report.adopted;
            break;

        case SweepVerdict::leave_alone:
            // Neither terminated nor adopted.  Counted because it is the answer
            // that needs a human: an orphan the sweep cannot attribute still holds
            // whatever device it was given.
            ++report.left_alone;
            break;

        case SweepVerdict::terminate_cooperatively: {
            ++report.asked_to_terminate;
            const auto asked = source.request_cooperative_exit(*process);
            if (asked.ok()) {
                // Delivery is not exit.  There is nothing to wait with here: the
                // retained handle belongs to the session that launched the worker,
                // and this sweep is looking at a process its predecessor owned.
                break;
            }
            if (!options.force_when_unreachable) {
                // The honest default, and the common case for a real orphan: a
                // previous session's worker cannot be reached over the private
                // channel, because that session's startup secret died with it.  So
                // "asked it down" is not available, and the sweep reports that
                // rather than pretending it asked.
                ++report.left_alone;
                if (report.first_failure.empty()) {
                    report.first_failure = asked.error().message;
                }
                break;
            }
            // Escalation, explicitly asked for.  Counted as forced, because that is
            // what happened to the process.
            ++report.forced_to_terminate;
            const auto escalated = source.terminate_process(*process);
            if (!escalated.ok()) {
                ++report.failed_to_terminate;
                if (report.first_failure.empty()) {
                    report.first_failure = escalated.error().message;
                }
            }
            break;
        }

        case SweepVerdict::terminate_forced: {
            ++report.forced_to_terminate;
            const auto ended = source.terminate_process(*process);
            if (!ended.ok()) {
                ++report.failed_to_terminate;
                if (report.first_failure.empty()) {
                    report.first_failure = ended.error().message;
                }
            }
            break;
        }
        }
    }

    return Result<WorkerSweepReport>::success(std::move(report));
}

} // namespace myrd::management
