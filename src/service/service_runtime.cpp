#include "src/service/service_runtime.h"

#include "src/management/windows_worker_job.h"
#include "src/profile/profile_store.h"

#include <filesystem>
#include <utility>

namespace myrd::service {
namespace {

Error runtime_error(const wchar_t* operation, const wchar_t* message,
                    const std::uint32_t native_code = 0) {
    return {ErrorDomain::storage, 1, operation, message, native_code};
}

} // namespace

Result<void> ProductionRepairBindingVerifier::verify_repair_binding(
    const profile::ProfileRecord& previous) {
    // Repair promotes the *previous* record back to current, so it restores a
    // claim about a bundle as it was.  If that bundle is gone, the repaired
    // profile names a disk that can be neither started nor removed -- a
    // registration to a bundle nobody has.  So the bundle has to still be a
    // bundle before its record may be restored.
    const std::wstring& bundle = previous.profile.bundle_path;
    if (bundle.empty()) {
        return Result<void>::failure(runtime_error(
            L"verify_repair_binding",
            L"the previous record names no bundle"));
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(bundle, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return Result<void>::failure(runtime_error(
            L"verify_repair_binding",
            L"the previous record's bundle could not be inspected",
            static_cast<std::uint32_t>(error.value())));
    }
    if (!std::filesystem::exists(status)
        || !std::filesystem::is_directory(status)) {
        return Result<void>::failure(runtime_error(
            L"verify_repair_binding",
            L"the previous record's bundle is not a directory"));
    }
    // The manifest is what makes a directory a bundle.  Checked by name rather
    // than parsed: the honest question on this path is "is this still the bundle
    // the record describes", and the bundle layer answers the deeper one when the
    // profile is next started.
    error.clear();
    const auto manifest = std::filesystem::symlink_status(
        std::filesystem::path(bundle) / previous.profile.manifest_name, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return Result<void>::failure(runtime_error(
            L"verify_repair_binding",
            L"the previous record's manifest could not be inspected",
            static_cast<std::uint32_t>(error.value())));
    }
    if (!std::filesystem::exists(manifest)
        || !std::filesystem::is_regular_file(manifest)) {
        return Result<void>::failure(runtime_error(
            L"verify_repair_binding",
            L"the previous record's manifest is absent"));
    }
    return Result<void>::success();
}

Result<std::unique_ptr<ServiceRuntime>> ServiceRuntime::create(
    ServiceRuntimeConfig config) {
    if (config.program_data_root.empty()) {
        return Result<std::unique_ptr<ServiceRuntime>>::failure(runtime_error(
            L"ServiceRuntime::create", L"no ProgramData root was configured"));
    }
    if (config.worker_executable.empty()) {
        return Result<std::unique_ptr<ServiceRuntime>>::failure(runtime_error(
            L"ServiceRuntime::create",
            L"no worker executable was configured"));
    }
    // Fail closed on an installation that is not there.  One check on the
    // product root rather than a chain of probes: the installer creates the roots
    // together, so "the product root is absent" is the honest summary.
    //
    // Reached only when the name exists.  When it does *not*, the first store's
    // own failure is what the operator gets and it is the more accurate one --
    // the store says which protected root it could not open, while a check here
    // could only say that a name is missing.  That distinction is why this is not
    // made a hard precondition: an install that is present but unusable must be
    // described by the component that found it unusable.
    std::error_code error;
    const std::filesystem::path product_root =
        std::filesystem::path(config.program_data_root) / L"MyRamDisk";
    if (std::filesystem::exists(product_root, error)
        && !std::filesystem::is_directory(product_root, error)) {
        return Result<std::unique_ptr<ServiceRuntime>>::failure(runtime_error(
            L"ServiceRuntime::create",
            L"the ProgramData product root exists and is not a directory, so "
            L"this machine's durable state cannot be opened",
            static_cast<std::uint32_t>(error.value())));
    }

    // Built first, so the stores below can borrow *its* clock and its verifier
    // rather than the caller's temporaries.
    std::unique_ptr<ServiceRuntime> runtime(
        new ServiceRuntime(std::move(config)));

    // Phase two: the durable roots, emplaced in dependency order.  The operation
    // store borrows this runtime's clock, which is why it is built here and not
    // from a factory temporary.
    auto registrations = management::ProfileRegistrationStore::open_existing(
        runtime->config_.program_data_root, runtime->repair_verifier_);
    if (!registrations.ok()) {
        return Result<std::unique_ptr<ServiceRuntime>>::failure(
            registrations.error());
    }
    runtime->registrations_.emplace(registrations.move_value());

    auto operations = management::ServiceOperationStore::open_existing(
        runtime->config_.program_data_root, runtime->clock_,
        runtime->wait_pace_);
    if (!operations.ok()) {
        return Result<std::unique_ptr<ServiceRuntime>>::failure(
            operations.error());
    }
    runtime->operations_.emplace(operations.move_value());

    // The worker job and its live registry, before anything that launches a
    // worker.  Both obligations of the job are impossible to add later: a worker
    // launched without one is invisible to a sweep, and a handle not retained at
    // launch cannot be recovered afterwards.
    auto processes = management::WindowsWorkerProcessSource::create();
    if (!processes.ok()) {
        return Result<std::unique_ptr<ServiceRuntime>>::failure(
            processes.error());
    }
    runtime->processes_ = processes.move_value();

    // The fingerprint store, before the provider that writes to it.  Failing here
    // fails the start rather than running without it: a session that cannot record
    // what it launched produces workers the next start cannot attribute, and the
    // sweep would then report a condition on every boot instead of cleaning up.
    auto bindings = management::WorkerBindingStore::open(
        runtime->config_.program_data_root);
    if (!bindings.ok()) {
        return Result<std::unique_ptr<ServiceRuntime>>::failure(
            bindings.error());
    }
    runtime->bindings_.emplace(bindings.move_value());

    runtime->sessions_.emplace(runtime->config_.worker_executable,
                               *runtime->processes_, &*runtime->bindings_);
    runtime->worker_.emplace(*runtime->sessions_,
                             runtime->config_.worker_control_timeout_ms,
                             &*runtime->bindings_);
    runtime->execution_gate_ = std::make_unique<std::timed_mutex>();

    // The sweep's two halves: the durable fingerprints as bindings, and the sweep
    // itself over the job.
    //
    // `expects_running_worker` is answered from the durable *profile* records, not
    // from the fingerprint -- a fingerprint says which process a profile last had
    // and nothing about whether that profile is supposed to be up.  The predicate
    // reads the registration store, and a profile whose record cannot be read is
    // reported as *not* expected running: that terminates a worker which might have
    // been meant to serve, which is the safer of the two mistakes, because the
    // alternative leaves two workers believing they own one bundle.
    auto* const registrations_for_sweep = &*runtime->registrations_;
    runtime->expected_bindings_.emplace(
        *runtime->bindings_,
        [registrations_for_sweep](const product::Uuid& profile_id) {
            auto snapshot = registrations_for_sweep->load(profile_id);
            if (!snapshot.ok()) return false;
            return snapshot.value().state.disk.runtime
                == product::RuntimeState::running;
        });
    runtime->resource_identities_.emplace(*runtime->registrations_);
    // The real reclaimer, composed after the two things it borrows.  It re-reads the
    // *same* registration store the sweep proved the identity from, which is what makes the
    // reclaim act on the profile as it is now rather than on the sweep's older reading of it.
    runtime->resource_reclaimer_.emplace(*runtime->registrations_, *runtime->worker_);
    // **Escalate to a forced end when an orphan cannot be reached (R-04.B).**
    //
    // A worker that outlived its service can never be *asked* to leave: the private
    // channel and its startup secret died with the service that created them, so
    // `request_cooperative_exit` cannot reach it -- by construction, not by accident.
    // With the default `force_when_unreachable = false` the sweep reports such a worker
    // as left alone, and that worker keeps the bundle's exclusive write lock and its AIM
    // device for as long as it lives.  Measured (R-04.B): after a service kill inside the
    // P4 window the worker survived, and the restarted service could not reconcile the
    // disk it was holding.
    //
    // The teardown the dead worker can no longer perform is the sweep's *resource half*,
    // which runs after the process half and reclaims the proven device/letter from the
    // durable identity.  So forcing the process end does not skip the ordered teardown;
    // it hands it to the component that was composed to do it without the worker.
    management::WorkerSweepOptions sweep_options{};
    sweep_options.force_when_unreachable = true;
    runtime->startup_sweep_.emplace(*runtime->processes_,
                                    *runtime->expected_bindings_,
                                    sweep_options,
                                    &*runtime->resource_identities_,
                                    &*runtime->resource_reclaimer_);

    // The core borrows every store above, so it is emplaced after them; the
    // scheduler borrows the core, so it is emplaced after that.
    runtime->core_.emplace(runtime->clock_, *runtime->registrations_,
                           *runtime->operations_, *runtime->worker_,
                           runtime->logger_, runtime->ids_, runtime->memory_,
                           &*runtime->registrations_);
    runtime->maintenance_.emplace(*runtime->core_, runtime->clock_,
                                  runtime->logger_, *runtime->execution_gate_);
    runtime->scheduler_.emplace(*runtime->core_, runtime->clock_,
                                *runtime->execution_gate_,
                                runtime->config_.scheduler,
                                &*runtime->maintenance_);
    // The public surface last, because it borrows the core and the gate -- and because
    // it is the thing the host opens last.  Emplaced here so the wiring is one
    // expression in dependency order, like everything above it.
    runtime->surface_.emplace(*runtime->core_, *runtime->execution_gate_,
                              runtime->config_.pipe_name);
    return Result<std::unique_ptr<ServiceRuntime>>::success(std::move(runtime));
}

ServiceRuntime::ServiceRuntime(ServiceRuntimeConfig config) noexcept
    : config_(std::move(config)) {}

ServiceRuntime::~ServiceRuntime() noexcept = default;

ServiceHost ServiceRuntime::host() {
    return ServiceHost(clock_, *core_, *scheduler_, config_.host,
                       startup_sweep_.has_value() ? &*startup_sweep_ : nullptr,
                       surface_.has_value() ? &*surface_ : nullptr,
                       maintenance_.has_value() ? &*maintenance_ : nullptr);
}

} // namespace myrd::service
