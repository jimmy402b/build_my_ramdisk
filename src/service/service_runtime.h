#pragma once

// The production composition of the service.
//
// One object owns every piece the service is made of, in the one order that
// makes the references between them valid, and hands out a `ServiceHost` built
// over them.  It exists because `ServiceCore` holds *references* to its stores,
// its worker control, its clock and its logger: those objects have to outlive
// it, and a wiring that built them as locals would leave a running scheduler
// pointing at destroyed state.  Owning them together is how that is made
// impossible rather than remembered.
//
// **Fail-closed on a machine that is not installed.**  The durable roots under
// `%ProgramData%` are created by the installer (T4-01), which is the component
// that also owns their ACLs; this wiring only ever opens what is already there.
// A service that created them itself would be putting a privileged write on its
// own start path, which is the one place it can least afford one -- and a
// half-created root is worse than an absent one, because it looks installed.

#include "src/common/result.h"
#include "src/management/execution_scheduler.h"
#include "src/management/profile_registration_store.h"
#include "src/management/service_core.h"
#include "src/management/service_operation_store.h"
#include "src/management/service_worker_control.h"
#include "src/management/windows_worker_sessions.h"
#include "src/management/worker_binding_store.h"
#include "src/service/public_pipe_surface.h"
#include "src/service/service_host.h"
#include "src/service/service_system.h"
#include "src/service/startup_sweep.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace myrd::service {

// The binding check a profile repair must pass.
//
// `ProfileStore::repair_from_previous` calls this before it promotes the
// *previous* record back to current, because the previous record is a record of
// a bundle as it was, and promoting it restores a claim about a bundle that may
// no longer be there.  A repair that restored a profile pointing at a bundle
// somebody deleted would manufacture a disk that cannot be started and cannot be
// removed -- so this refuses when the bundle the record names is no longer a
// bundle.
class ProductionRepairBindingVerifier final
    : public profile::IProfileRepairBindingVerifier {
public:
    [[nodiscard]] Result<void> verify_repair_binding(
        const profile::ProfileRecord& previous) override;
};

struct ServiceRuntimeConfig final {
    // The trusted `%ProgramData%` root.  Passed in rather than derived so a test
    // -- or a future installer check -- can point it somewhere else.
    std::wstring program_data_root;
    // The per-profile worker executable.  Where it lives is an installation
    // question, and a wiring that guessed would be a second place to be wrong.
    std::wstring worker_executable;
    ServiceHostOptions host{};
    management::ExecutionSchedulerConfig scheduler{};
    std::uint32_t worker_control_timeout_ms{
        management::kDefaultWorkerControlTimeoutMs};
    // The public control pipe's name.  Empty means the documented production name;
    // an end-to-end run passes a private one so it does not compete with an installed
    // service, exactly as the client's `--pipe` does.
    std::wstring pipe_name;
};

// Everything the service is made of, owned in one place.
//
// **Not movable, and returned as a `unique_ptr`, because it cannot be.**  It
// holds a scheduler that borrows the core and a core that borrows everything
// else; a move would leave those references pointing at the moved-from object,
// and the compiler would not say a word -- it would be a dangling reference that
// only shows up as a crash under load.  Heap ownership with one constructor is
// how the wiring stays a single expression that cannot be reordered by accident.
class ServiceRuntime final {
public:
    // Opens the durable roots and wires the service over them.  Fails when the
    // installation is absent or unusable -- see the header comment; nothing here
    // creates it.
    [[nodiscard]] static Result<std::unique_ptr<ServiceRuntime>> create(
        ServiceRuntimeConfig config);

    ~ServiceRuntime() noexcept;
    ServiceRuntime(const ServiceRuntime&) = delete;
    ServiceRuntime& operator=(const ServiceRuntime&) = delete;
    ServiceRuntime(ServiceRuntime&&) = delete;
    ServiceRuntime& operator=(ServiceRuntime&&) = delete;

    // The service's lifecycle, over exactly these objects.  Returned by value:
    // `ServiceHost` has a hand-written move precisely so a caller can hold it,
    // and a value here keeps the ownership story in one place.
    [[nodiscard]] ServiceHost host();

    [[nodiscard]] const ServiceRuntimeConfig& config() const noexcept {
        return config_;
    }

private:
    explicit ServiceRuntime(ServiceRuntimeConfig config) noexcept;

    ServiceRuntimeConfig config_{};

    // Every member that cannot be constructed in the initializer list is held in
    // an `optional` and emplaced by `create`, in dependency order.  The reasons
    // are different for each, and all three are real:
    //
    //   * `core_` -- the scheduler borrows it and it borrows the scheduler's
    //     dependencies, so its declaration has to come early and its construction
    //     late;
    //   * `registrations_` / `operations_` -- no default constructor, and the
    //     operation store borrows this object's clock, so it cannot be built from
    //     a factory temporary;
    //   * `scheduler_` -- borrows the core.
    //
    // A plain member can satisfy at most one of those at a time.
    SystemClock clock_{};
    ProductionRepairBindingVerifier repair_verifier_{};
    management::SystemOperationWaitPace wait_pace_{};
    std::optional<management::ServiceCore> core_;
    std::optional<management::ProfileRegistrationStore> registrations_;
    std::optional<management::ServiceOperationStore> operations_;
    // The worker job and its live registry, declared before the two things that
    // borrow it.  The session provider places every launch into this job and
    // retains the process handle there, so a provider built without it could
    // launch workers nothing could ever find or stop.
    std::unique_ptr<management::WindowsWorkerProcessSource> processes_;
    std::optional<management::WindowsWorkerSessionProvider> sessions_;
    std::optional<management::ServiceWorkerControl> worker_;
    // Where a launched worker's fingerprint is recorded, so the next start can
    // tell that worker from a process that merely inherited its pid.  Declared
    // before the provider, which borrows it.
    std::optional<management::WorkerBindingStore> bindings_;
    // The start-up sweep and the two pieces it needs.  Declared after the store it
    // reads and after the job it enumerates, both of which it borrows.
    std::optional<service::FingerprintWorkerBindings> expected_bindings_;
    // The resource half of the sweep: the durable identities, and the reclaimer that
    // currently reports it cannot act.  Declared before the sweep that borrows both.
    std::optional<service::ProfileResourceIdentities> resource_identities_;
    // The reclaimer is now the real one, so a *proven* leftover is dismantled rather than
    // only reported.  `DeferredResourceReclaimer` is kept as the honest option for a service
    // composed without a worker adapter; it is still the type a test uses to exercise the
    // "cannot reclaim" branch, which has to keep working because it is the branch an
    // operator sees whenever reclamation is unavailable.
    //
    // It borrows the registrations and the worker, and is emplaced in that order because
    // both must outlive it.
    std::optional<service::LeftoverReclaimer> resource_reclaimer_;
    std::optional<service::ProductionStartupSweep> startup_sweep_;
    StderrServiceLogger logger_{};
    SystemIdGenerator ids_{};
    SystemPhysicalMemoryQuery memory_{};
    // The gate the pipe and the scheduler share.  Nothing contends on it yet
    // because the public pipe is T1-05; it is here now so the scheduler is built
    // against the gate it will actually be serialized by, rather than against a
    // temporary one it would have to be rebuilt with later.
    //
    // Timed rather than plain, because the pipe takes it with a deadline: the
    // scheduler holds it across a whole operation, and a command that arrives
    // behind one must be answered with a timeout instead of being parked.
    std::unique_ptr<std::timed_mutex> execution_gate_;
    // The public command surface, over the core and the gate above.  Declared after
    // both, which it borrows, and before the host that borrows it.  Emplaced in
    // `create` rather than default-constructed because it borrows the core, which is
    // itself emplaced late.
    std::optional<PublicPipeSurface> surface_;
    std::optional<management::ServiceExecutionScheduler> scheduler_;
};

} // namespace myrd::service
