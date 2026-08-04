#include "m2_lifecycle_runner.h"

#include <array>
#include <ctime>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace myrd {
namespace {

constexpr std::array<M2PhaseId, 16> kPhaseOrder = {
    M2PhaseId::capture_baseline,
    M2PhaseId::construct_provider,
    M2PhaseId::start_provider,
    M2PhaseId::create_device,
    M2PhaseId::wait_for_disk,
    M2PhaseId::setup_write,
    M2PhaseId::sample_after_write,
    M2PhaseId::delete_retrim,
    M2PhaseId::sample_after_retrim,
    M2PhaseId::performance,
    M2PhaseId::remove_mounts,
    M2PhaseId::remove_device,
    M2PhaseId::stop_provider,
    M2PhaseId::restore_service,
    M2PhaseId::compare_baseline,
    M2PhaseId::publish_report,
};

constexpr std::array<M2CleanupActionId, 4> kCleanupOrder = {
    M2CleanupActionId::remove_mounts,
    M2CleanupActionId::remove_device,
    M2CleanupActionId::stop_provider,
    M2CleanupActionId::restore_service,
};

std::string narrow_ascii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
        result.push_back(ch >= 0 && ch <= 0x7f
                             ? static_cast<char>(ch)
                             : '?');
    }
    return result;
}

std::string error_message(const Error& error) {
    auto message = narrow_ascii(error.operation);
    if (!message.empty() && !error.message.empty()) message += ": ";
    message += narrow_ascii(error.message);
    if (message.empty()) message = "operation failed";
    return message;
}

Error cancelled_error(M2PhaseId phase) {
    return {
        ErrorDomain::storage,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled),
        std::wstring(to_string(phase).begin(), to_string(phase).end()),
        L"operation cancelled"};
}

Error timeout_error(M2PhaseId phase) {
    return {
        ErrorDomain::storage,
        static_cast<std::uint32_t>(OperationErrorCode::timed_out),
        std::wstring(to_string(phase).begin(), to_string(phase).end()),
        L"operation deadline expired"};
}

Error exception_error(M2PhaseId phase) {
    return {
        ErrorDomain::internal,
        static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
        std::wstring(to_string(phase).begin(), to_string(phase).end()),
        L"unexpected exception"};
}

Error evidence_error(M2PhaseId phase, std::wstring message) {
    return {
        ErrorDomain::evidence,
        static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
        std::wstring(to_string(phase).begin(), to_string(phase).end()),
        std::move(message)};
}

std::wstring describe_baseline_mismatch(
    const M2FinalBaselineComparison& comparison) {
    std::wostringstream message;
    message << L"final baseline does not match"
            << L"; exact_match=" << std::boolalpha
            << comparison.exact_match
            << L"; resources_within_tolerance="
            << comparison.resources_within_tolerance;
    const auto append_values = [&message](
        std::wstring_view label,
        const std::vector<std::string>& values) {
        message << L"; " << label << L"=[";
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0) message << L", ";
            message << std::wstring(values[index].begin(), values[index].end());
        }
        message << L']';
    };
    append_values(L"added", comparison.diff.added);
    append_values(L"removed", comparison.diff.removed);
    append_values(L"changed", comparison.diff.changed);
    message << L"; handles=" << comparison.before.process_handle_count
            << L"->" << comparison.after.process_handle_count
            << L"; threads=" << comparison.before.process_thread_count
            << L"->" << comparison.after.process_thread_count
            << L"; private_bytes="
            << comparison.before.process_private_bytes << L"->"
            << comparison.after.process_private_bytes
            << L"; private_bytes_tolerance="
            << comparison.private_bytes_tolerance;
    return message.str();
}

class LifecycleRunner {
public:
    LifecycleRunner(
        IM2BaselineControl& baseline,
        IM2LifecycleProviderFactory& providers,
        IM2DeviceControl& devices,
        IM2VolumeWorkload& workload,
        IM2ReportPublisher& publisher,
        IM2LifecycleClock& clock,
        M2LifecycleConfig config)
        : baseline_(baseline),
          providers_(providers),
          devices_(devices),
          workload_(workload),
          publisher_(publisher),
          clock_(clock),
          config_(std::move(config)),
          report_(std::move(config_.report)) {}

    M2Report run() {
        reset_dynamic_evidence();
        execute_main_path();
        execute_cleanup();
        compare_baseline();
        publish_report();
        return std::move(report_);
    }

private:
    struct PhaseExecution {
        M2Phase& phase;
        OperationContext context;
        std::chrono::steady_clock::time_point started;
    };

    template <typename T>
    struct Acquisition {
        T value;
        bool phase_succeeded{};
    };

    void reset_dynamic_evidence() {
        report_.run.started_utc = clock_.utc_now();
        report_.run.ended_utc.clear();
        report_.state.durable_successor_installed = false;
        report_.state.durable_successor_generation_present = false;
        report_.state.durable_successor_generation = 0;
        report_.state.durability_scope_statement =
            std::string(kM2NoDurableSuccessorStatement);
        report_.phases.clear();
        report_.phases.reserve(kPhaseOrder.size());
        for (const auto id : kPhaseOrder) {
            report_.phases.push_back(M2Phase{id});
        }
        report_.io = {};
        report_.store_stats_samples.clear();
        report_.os_memory_samples.clear();
        report_.performance = {};
        report_.device_identity = {};
        report_.cleanup_actions.clear();
        report_.cleanup_actions.reserve(kCleanupOrder.size());
        for (const auto id : kCleanupOrder) {
            report_.cleanup_actions.push_back(M2CleanupAction{id});
        }
        report_.state_before = {};
        report_.state_after = {};
        report_.final_baseline_comparison = {};
        report_.failures.clear();
        report_.verdict = M2Verdict::incomplete;
        run_started_steady_ = clock_.steady_now();
    }

    M2Phase& phase(M2PhaseId id) {
        return report_.phases[static_cast<std::size_t>(id)];
    }

    M2CleanupAction& cleanup(M2CleanupActionId id) {
        return report_.cleanup_actions[static_cast<std::size_t>(id)];
    }

    PhaseExecution begin_phase(M2PhaseId id, bool cleanup_phase) {
        auto& value = phase(id);
        value.started_utc = clock_.utc_now();
        const auto started = clock_.steady_now();
        OperationContext context;
        context.stop_token =
            cleanup_phase ? std::stop_token{} : config_.stop_token;
        context.deadline = started + config_.phase_timeout;
        return {value, context, started};
    }

    std::optional<Error> context_error(
        M2PhaseId id,
        const OperationContext& context) {
        if (context.stop_token.stop_requested()) {
            return cancelled_error(id);
        }
        if (clock_.steady_now() >= context.deadline) {
            return timeout_error(id);
        }
        return std::nullopt;
    }

    void finish_phase_pass(PhaseExecution& execution) {
        execution.phase.ended_utc = clock_.utc_now();
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                clock_.steady_now() - execution.started);
        execution.phase.elapsed_ms =
            elapsed.count() < 0 ? 0 : static_cast<std::uint64_t>(
                elapsed.count());
        execution.phase.status = M2PhaseStatus::passed;
        execution.phase.error.clear();
    }

    void finish_phase_failure(
        M2PhaseId id,
        PhaseExecution& execution,
        const Error& error) {
        execution.phase.ended_utc = clock_.utc_now();
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                clock_.steady_now() - execution.started);
        execution.phase.elapsed_ms =
            elapsed.count() < 0 ? 0 : static_cast<std::uint64_t>(
                elapsed.count());
        execution.phase.status = M2PhaseStatus::failed;
        execution.phase.error = error_message(error);
        report_.failures.push_back({
            std::string(to_string(id)), error.code,
            execution.phase.error});
    }

    void replace_pass_with_evidence_failure(
        M2PhaseId id,
        std::wstring message) {
        auto& value = phase(id);
        const auto error = evidence_error(id, std::move(message));
        value.status = M2PhaseStatus::failed;
        value.error = error_message(error);
        report_.failures.push_back({
            std::string(to_string(id)), error.code, value.error});
    }

    bool inject_failure_after(M2PhaseId id) {
        if (!config_.injected_failure_after ||
            *config_.injected_failure_after != id) {
            return false;
        }
        replace_pass_with_evidence_failure(
            id, L"injected acceptance failure after successful phase");
        return true;
    }

    template <typename T, typename Function>
    std::optional<T> execute_value(
        M2PhaseId id,
        bool cleanup_phase,
        Function&& function) {
        auto execution = begin_phase(id, cleanup_phase);
        if (auto error = context_error(id, execution.context)) {
            finish_phase_failure(id, execution, *error);
            return std::nullopt;
        }
        try {
            auto result = function(execution.context);
            if (!result) {
                finish_phase_failure(id, execution, result.error());
                return std::nullopt;
            }
            if (auto error = context_error(id, execution.context)) {
                finish_phase_failure(id, execution, *error);
                return std::nullopt;
            }
            auto value = result.move_value();
            finish_phase_pass(execution);
            return std::optional<T>(std::move(value));
        } catch (...) {
            const auto error = exception_error(id);
            finish_phase_failure(id, execution, error);
            return std::nullopt;
        }
    }

    template <typename Function>
    bool execute_void(
        M2PhaseId id,
        bool cleanup_phase,
        Function&& function) {
        auto execution = begin_phase(id, cleanup_phase);
        if (auto error = context_error(id, execution.context)) {
            finish_phase_failure(id, execution, *error);
            return false;
        }
        try {
            auto result = function(execution.context);
            if (!result) {
                finish_phase_failure(id, execution, result.error());
                return false;
            }
            if (auto error = context_error(id, execution.context)) {
                finish_phase_failure(id, execution, *error);
                return false;
            }
            finish_phase_pass(execution);
            return true;
        } catch (...) {
            const auto error = exception_error(id);
            finish_phase_failure(id, execution, error);
            return false;
        }
    }

    template <typename T, typename Function>
    std::optional<Acquisition<T>> execute_acquisition(
        M2PhaseId id,
        Function&& function) {
        auto execution = begin_phase(id, false);
        if (auto error = context_error(id, execution.context)) {
            finish_phase_failure(id, execution, *error);
            return std::nullopt;
        }
        try {
            auto result = function(execution.context);
            if (!result) {
                finish_phase_failure(id, execution, result.error());
                return std::nullopt;
            }
            auto value = result.move_value();
            if (auto error = context_error(id, execution.context)) {
                finish_phase_failure(id, execution, *error);
                return Acquisition<T>{std::move(value), false};
            }
            finish_phase_pass(execution);
            return Acquisition<T>{std::move(value), true};
        } catch (...) {
            const auto error = exception_error(id);
            finish_phase_failure(id, execution, error);
            return std::nullopt;
        }
    }

    void append_sample(
        M2StoreSamplePoint point,
        M2PhaseId phase_id,
        M2ProviderSample sample) {
        const auto captured = clock_.utc_now();
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                clock_.steady_now() - run_started_steady_);
        const auto elapsed_ms =
            elapsed.count() < 0 ? 0 : static_cast<std::uint64_t>(
                elapsed.count());
        report_.store_stats_samples.push_back(
            {point, captured, std::move(sample.store_stats)});
        report_.os_memory_samples.push_back({
            point,
            phase_id,
            captured,
            sample.provider_process_id,
            elapsed_ms,
            sample.working_set_bytes,
            sample.private_bytes,
            sample.commit_bytes});
    }

    void execute_main_path() {
        auto baseline = execute_value<M2BaselineSnapshot>(
            M2PhaseId::capture_baseline, false,
            [this](const OperationContext& context) {
                return baseline_.capture(context);
            });
        if (!baseline) return;
        baseline_snapshot_ = std::move(*baseline);
        report_.state_before = baseline_snapshot_->driver_service;
        report_.final_baseline_comparison.before =
            baseline_snapshot_->inventory;
        report_.final_baseline_comparison.before_fingerprint =
            baseline_snapshot_->fingerprint;
        restore_service_owned_ = true;
        if (inject_failure_after(M2PhaseId::capture_baseline)) return;

        auto provider = execute_acquisition<
            std::unique_ptr<IM2LifecycleProvider>>(
            M2PhaseId::construct_provider,
            [this](const OperationContext& context) {
                return providers_.create(
                    context, report_.geometry_options, report_.state);
            });
        if (!provider) return;
        if (!provider->value) {
            if (provider->phase_succeeded) {
                replace_pass_with_evidence_failure(
                    M2PhaseId::construct_provider,
                    L"provider factory returned a null provider");
            }
            return;
        }
        provider_ = std::move(provider->value);
        provider_owned_ = true;
        if (!provider->phase_succeeded) return;
        if (inject_failure_after(M2PhaseId::construct_provider)) return;

        auto ready_sample = execute_value<M2ProviderSample>(
            M2PhaseId::start_provider, false,
            [this](const OperationContext& context)
                -> Result<M2ProviderSample> {
                auto started = provider_->start(context);
                if (!started) {
                    return Result<M2ProviderSample>::failure(started.error());
                }
                return provider_->sample(
                    context, M2StoreSamplePoint::ready);
            });
        if (!ready_sample) return;
        append_sample(
            M2StoreSamplePoint::ready,
            M2PhaseId::start_provider,
            std::move(*ready_sample));
        if (inject_failure_after(M2PhaseId::start_provider)) return;

        auto owned = execute_acquisition<M2OwnedDevice>(
            M2PhaseId::create_device,
            [this](const OperationContext& context) {
                return devices_.create(
                    context, provider_->native_object_path(),
                    report_.geometry_options);
            });
        if (!owned) return;
        if (owned->value.id.value == aim::kAllDevices ||
            owned->value.id.value == aim::kAutoDeviceNumber) {
            if (owned->phase_succeeded) {
                replace_pass_with_evidence_failure(
                    M2PhaseId::create_device,
                    L"device control returned an unsafe DeviceId");
            }
            return;
        }
        owned_device_ = std::move(owned->value);
        device_owned_ = true;
        report_.device_identity = owned_device_->identity;
        if (!owned->phase_succeeded) return;
        if (inject_failure_after(M2PhaseId::create_device)) return;

        auto identity = execute_value<M2DeviceIdentity>(
            M2PhaseId::wait_for_disk, false,
            [this](const OperationContext& context) {
                return devices_.wait_for_disk(context, *owned_device_);
            });
        if (!identity) return;
        report_.device_identity = std::move(*identity);
        if (inject_failure_after(M2PhaseId::wait_for_disk)) return;

        auto io = execute_value<M2IoEvidence>(
            M2PhaseId::setup_write, false,
            [this](const OperationContext& context)
                -> Result<M2IoEvidence> {
                bool callback_invoked = false;
                auto result = workload_.setup_and_write(
                    context, report_.device_identity,
                    [this, &callback_invoked]() {
                        callback_invoked = true;
                        mounts_owned_ = true;
                    });
                if (!result) return result;
                if (!callback_invoked || !mounts_owned_) {
                    return Result<M2IoEvidence>::failure(evidence_error(
                        M2PhaseId::setup_write,
                        L"workload succeeded without mount ownership handoff"));
                }
                return result;
            });
        if (!io) return;
        report_.io = std::move(*io);
        if (inject_failure_after(M2PhaseId::setup_write)) return;

        auto after_write = execute_value<M2ProviderSample>(
            M2PhaseId::sample_after_write, false,
            [this](const OperationContext& context) {
                return provider_->sample(
                    context, M2StoreSamplePoint::after_write);
            });
        if (!after_write) return;
        append_sample(
            M2StoreSamplePoint::after_write,
            M2PhaseId::sample_after_write,
            std::move(*after_write));
        if (inject_failure_after(M2PhaseId::sample_after_write)) return;

        auto deleted = execute_value<std::uint64_t>(
            M2PhaseId::delete_retrim, false,
            [this](const OperationContext& context) {
                return workload_.delete_and_retrim(
                    context, report_.device_identity);
            });
        if (!deleted) return;
        report_.io.bytes_deleted = *deleted;
        if (inject_failure_after(M2PhaseId::delete_retrim)) return;

        auto after_retrim = execute_value<M2ProviderSample>(
            M2PhaseId::sample_after_retrim, false,
            [this](const OperationContext& context) {
                return provider_->sample(
                    context, M2StoreSamplePoint::after_retrim);
            });
        if (!after_retrim) return;
        append_sample(
            M2StoreSamplePoint::after_retrim,
            M2PhaseId::sample_after_retrim,
            std::move(*after_retrim));
        if (inject_failure_after(M2PhaseId::sample_after_retrim)) return;

        auto performance = execute_value<M2PerformanceEvidence>(
            M2PhaseId::performance, false,
            [this](const OperationContext& context)
                -> Result<M2PerformanceEvidence> {
                auto measured = workload_.run_performance(
                    context, report_.device_identity);
                if (!measured) return measured;
                if (measured.value().gate_status !=
                        M2PerformanceGateStatus::passed ||
                    !measured.value().comparison.overall_pass) {
                    report_.performance = measured.value();
                    return Result<M2PerformanceEvidence>::failure(
                        evidence_error(
                            M2PhaseId::performance,
                            L"performance gate did not pass"));
                }
                return measured;
            });
        if (!performance) return;
        report_.performance = std::move(*performance);
        (void)inject_failure_after(M2PhaseId::performance);
    }

    void set_cleanup_result(
        M2CleanupActionId id,
        bool succeeded,
        const M2Phase& source_phase) {
        auto& action = cleanup(id);
        action.attempted = true;
        action.result = succeeded
                            ? M2CleanupResult::succeeded
                            : M2CleanupResult::failed;
        action.error = succeeded ? std::string{} : source_phase.error;
    }

    void execute_cleanup() {
        if (mounts_owned_) {
            const auto succeeded = execute_void(
                M2PhaseId::remove_mounts, true,
                [this](const OperationContext& context) {
                    return workload_.remove_mounts(
                        context, report_.device_identity);
                });
            set_cleanup_result(
                M2CleanupActionId::remove_mounts,
                succeeded,
                phase(M2PhaseId::remove_mounts));
            if (succeeded) mounts_owned_ = false;
        }

        if (device_owned_ && owned_device_) {
            const auto succeeded = execute_void(
                M2PhaseId::remove_device, true,
                [this](const OperationContext& context) {
                    return devices_.remove(context, owned_device_->id);
                });
            set_cleanup_result(
                M2CleanupActionId::remove_device,
                succeeded,
                phase(M2PhaseId::remove_device));
            if (succeeded) device_owned_ = false;
        }

        if (provider_owned_ && provider_) {
            auto final_sample = execute_value<M2ProviderSample>(
                M2PhaseId::stop_provider, true,
                [this](const OperationContext& context)
                    -> Result<M2ProviderSample> {
                    auto stopped = provider_->stop(context);
                    if (!stopped) {
                        return Result<M2ProviderSample>::failure(
                            stopped.error());
                    }
                    return provider_->sample(
                        context, M2StoreSamplePoint::final);
                });
            const auto succeeded = final_sample.has_value();
            set_cleanup_result(
                M2CleanupActionId::stop_provider,
                succeeded,
                phase(M2PhaseId::stop_provider));
            if (succeeded) {
                append_sample(
                    M2StoreSamplePoint::final,
                    M2PhaseId::stop_provider,
                    std::move(*final_sample));
                provider_owned_ = false;
            }
        }
        // Baseline comparison measures live process resources. A stopped
        // provider is no longer needed after its FINAL sample, so destroy the
        // wrapper before taking that measurement instead of retaining any
        // implementation-owned handles until LifecycleRunner destruction.
        provider_.reset();

        if (restore_service_owned_ && baseline_snapshot_) {
            const auto succeeded = execute_void(
                M2PhaseId::restore_service, true,
                [this](const OperationContext& context) {
                    return baseline_.restore_service(
                        context, *baseline_snapshot_);
                });
            set_cleanup_result(
                M2CleanupActionId::restore_service,
                succeeded,
                phase(M2PhaseId::restore_service));
            if (succeeded) restore_service_owned_ = false;
        }
    }

    void compare_baseline() {
        if (!baseline_snapshot_) return;
        auto comparison = execute_value<M2BaselineComparisonEvidence>(
            M2PhaseId::compare_baseline, true,
            [this](const OperationContext& context) {
                return baseline_.compare(context, *baseline_snapshot_);
            });
        if (!comparison) return;
        report_.state_after =
            std::move(comparison->driver_service_after);
        report_.final_baseline_comparison =
            std::move(comparison->comparison);
        if (report_.final_baseline_comparison.comparison !=
                M2BaselineComparison::match ||
            !report_.final_baseline_comparison.exact_match ||
            !report_.final_baseline_comparison.resources_within_tolerance) {
            auto& compare_phase = phase(M2PhaseId::compare_baseline);
            const auto error = evidence_error(
                M2PhaseId::compare_baseline,
                describe_baseline_mismatch(
                    report_.final_baseline_comparison));
            compare_phase.status = M2PhaseStatus::failed;
            compare_phase.error = error_message(error);
            report_.failures.push_back({
                std::string(to_string(M2PhaseId::compare_baseline)),
                error.code, compare_phase.error});
        }
    }

    bool runtime_success() const {
        if (!report_.failures.empty()) return false;
        for (const auto& value : report_.phases) {
            if (value.id == M2PhaseId::publish_report) continue;
            if (value.status != M2PhaseStatus::passed) return false;
        }
        for (const auto& action : report_.cleanup_actions) {
            if (!action.attempted ||
                action.result != M2CleanupResult::succeeded) {
                return false;
            }
        }
        return report_.performance.gate_status ==
                   M2PerformanceGateStatus::passed &&
            report_.final_baseline_comparison.comparison ==
                M2BaselineComparison::match &&
            report_.final_baseline_comparison.exact_match &&
            report_.final_baseline_comparison.resources_within_tolerance;
    }

    void fail_publish_phase(
        PhaseExecution& execution,
        const Error& error) {
        finish_phase_failure(
            M2PhaseId::publish_report, execution, error);
        report_.verdict = M2Verdict::no_go;
        report_.run.ended_utc = execution.phase.ended_utc;
    }

    void publish_report() {
        auto execution = begin_phase(M2PhaseId::publish_report, true);
        if (auto error = context_error(
                M2PhaseId::publish_report, execution.context)) {
            fail_publish_phase(execution, *error);
            return;
        }

        report_.verdict =
            runtime_success() ? M2Verdict::go : M2Verdict::no_go;
        finish_phase_pass(execution);
        report_.run.ended_utc = execution.phase.ended_utc;

        auto semantic = validate_m2_report_json(
            serialize_m2_report(report_));
        if (!semantic) {
            std::wstring detail = L"report semantic validation failed";
            if (!semantic.error().message.empty()) {
                detail += L": ";
                detail += semantic.error().message;
            }
            const auto error = evidence_error(
                M2PhaseId::publish_report,
                std::move(detail));
            execution.phase.status = M2PhaseStatus::failed;
            execution.phase.error = error_message(error);
            report_.failures.push_back({
                std::string(to_string(M2PhaseId::publish_report)),
                error.code, execution.phase.error});
            report_.verdict = M2Verdict::no_go;

            // A GO-only completeness error can become a valid fail-closed
            // report after the publication failure is recorded. Static
            // schema/configuration corruption cannot; never hand such an
            // invalid artifact to a publisher.
            semantic = validate_m2_report_json(
                serialize_m2_report(report_));
            if (!semantic) return;
        }

        try {
            auto published = publisher_.publish(
                execution.context, report_);
            if (!published) {
                fail_publish_phase(execution, published.error());
                return;
            }
        } catch (...) {
            const auto error =
                exception_error(M2PhaseId::publish_report);
            fail_publish_phase(execution, error);
        }
    }

    IM2BaselineControl& baseline_;
    IM2LifecycleProviderFactory& providers_;
    IM2DeviceControl& devices_;
    IM2VolumeWorkload& workload_;
    IM2ReportPublisher& publisher_;
    IM2LifecycleClock& clock_;
    M2LifecycleConfig config_;
    M2Report report_;
    std::chrono::steady_clock::time_point run_started_steady_{};
    std::optional<M2BaselineSnapshot> baseline_snapshot_;
    std::unique_ptr<IM2LifecycleProvider> provider_;
    std::optional<M2OwnedDevice> owned_device_;
    bool restore_service_owned_{};
    bool provider_owned_{};
    bool device_owned_{};
    bool mounts_owned_{};
};

} // namespace

std::chrono::steady_clock::time_point
M2SystemLifecycleClock::steady_now() {
    return std::chrono::steady_clock::now();
}

std::string M2SystemLifecycleClock::utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (gmtime_s(&utc, &value) != 0) return {};
    char text[32]{};
    if (std::strftime(
            text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
    }
    return text;
}

M2Report run_m2_lifecycle(
    IM2BaselineControl& baseline,
    IM2LifecycleProviderFactory& providers,
    IM2DeviceControl& devices,
    IM2VolumeWorkload& workload,
    IM2ReportPublisher& publisher,
    IM2LifecycleClock& clock,
    M2LifecycleConfig config) {
    return LifecycleRunner(
               baseline, providers, devices, workload, publisher, clock,
               std::move(config))
        .run();
}

} // namespace myrd
