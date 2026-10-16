#include "src/management/worker_sweep.h"

#include <utility>

namespace myrd::management {

Result<WorkerSweepReport> run_worker_sweep(
    IWorkerProcessSource& source, const IExpectedWorkerBindings& bindings,
    const WorkerSweepOptions& options,
    const IObservedResourceIdentitySource* const identities,
    IResourceReclaimer* const reclaimer) {
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

    // The resource half, and the reason it is here rather than nowhere: this is the
    // only caller `reclaimable_resources` has, and a decision no code consults is the
    // "no consumer" defect this project has recorded three times.
    //
    // It runs *after* the process half on purpose.  Terminating a worker and then
    // reclaiming what it left is the order that cannot reclaim a device a live worker
    // is still using; the reverse would race the worker for its own volume.
    if (identities != nullptr) {
        auto identities_result = identities->observed_identities();
        if (!identities_result.ok()) {
            // Fatal, for the same reason an unreadable binding store is: with no
            // identities, every profile's leftovers would look unprovable and a
            // sweep that proceeded would report every disk as needing attention --
            // or, worse, a caller that read "unprovable" as "nothing to do" would
            // silently do nothing at all.
            return Result<WorkerSweepReport>::failure(identities_result.error());
        }

        for (const auto& [profile_id, identity] : identities_result.value()) {
            // The one call that turns an observation into permission.  `nullopt`
            // means "cannot prove ownership", which design section 5.5 requires be
            // *reported* rather than acted on.
            const auto allowed = reclaimable_resources(identity);
            if (!allowed.has_value()) {
                ++report.unprovable_profiles;
                continue;
            }

            ++report.reclaimable_profiles;
            if (reclaimer == nullptr || !reclaimer->can_reclaim()) {
                // Proven, and there is nothing here that can act on it.  Counted as
                // still needing attention rather than as done: this is precisely the
                // state T1-06 fills in, and reporting it as success would be claiming
                // a cleanup that did not happen.
                ++report.unprovable_profiles;
                if (report.first_failure.empty()) {
                    report.first_failure =
                        L"the resources of a profile are provably its own, but this "
                        L"service has no way to reclaim them yet";
                }
                continue;
            }

            auto reclaimed = reclaimer->reclaim(profile_id, *allowed);
            if (!reclaimed.ok()) {
                ++report.unprovable_profiles;
                if (report.first_failure.empty()) {
                    report.first_failure = reclaimed.error().message;
                }
                continue;
            }
            if (!reclaimed.value()) {
                // It ran and did not succeed.  Reported, never silently dropped.
                ++report.unprovable_profiles;
                if (report.first_failure.empty()) {
                    report.first_failure =
                        L"a proven resource could not be reclaimed";
                }
            }
        }
    }

    return Result<WorkerSweepReport>::success(std::move(report));
}

} // namespace myrd::management
