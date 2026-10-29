#include "src/management/windows_worker_sessions.h"

#include "src/host/m5_worker_private_pipe.h"
#include "src/management/windows_worker_job.h"
#include "src/management/worker_binding_store.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <utility>

namespace myrd::management {
namespace {

Error provider_error(const wchar_t* operation, const wchar_t* message,
                     const std::uint32_t native_code = 0) {
    return {ErrorDomain::internal,
            static_cast<std::uint32_t>(ServiceWorkerControlError::execution_not_bound),
            operation, message, native_code};
}

// The launch request's image uuid is a product uuid and the launch spec carries
// the persistence layer's sixteen raw bytes.  The bytes are the same sixteen in
// the same order -- `Uuid` stores its canonical form's bytes directly -- so this
// is a copy rather than a conversion, and going through text here would be a
// parse that can only fail.
persistence::ImageUuid to_image_uuid(const product::Uuid& id) noexcept {
    persistence::ImageUuid value{};
    const auto bytes = id.bytes();
    std::memcpy(value.bytes.data(), bytes.data(), value.bytes.size());
    return value;
}

} // namespace

WindowsWorkerSessionProvider::WindowsWorkerSessionProvider(
    std::wstring worker_executable,
    WindowsWorkerProcessSource& processes,
    WorkerBindingStore* const bindings) noexcept
    : worker_executable_(std::move(worker_executable)), processes_(processes),
      bindings_(bindings) {}

Result<std::unique_ptr<ServiceWorkerSession>>
WindowsWorkerSessionProvider::launch(const WorkerLaunchRequest& request,
                                     const OperationContext& context) {
    if (worker_executable_.empty()) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            provider_error(L"WindowsWorkerSessionProvider::launch",
                           L"the worker executable is not configured"));
    }

    const auto session_generation = next_session_generation_++;
    auto secret = m5::StartupSecret::generate();
    if (!secret.ok()) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            secret.error());
    }

    m5::WorkerLaunchSpec spec{request.profile_id, request.profile_revision};
    spec.volume_guid = request.volume_guid;
    spec.volume_label = request.volume_label;
    spec.ram_limit_bytes = request.ram_limit_bytes;
    spec.bundle_path = request.bundle_path;
    // Absent for an import, and absent is a different instruction from a zero
    // uuid: the worker adopts whatever the bundle holds instead of refusing one
    // that is not the image it was told to expect.
    if (request.image_id.has_value()) {
        spec.image_id = to_image_uuid(*request.image_id);
    }
    spec.volume_mode = request.volume_mode;
    spec.logical_size_bytes = request.logical_capacity_bytes;
    spec.mount_target = request.mount_target;
    spec.session_generation = session_generation;
    spec.private_pipe_name =
        m5::private_pipe_name(request.profile_id, session_generation);
    // A value this session generates and the worker must echo.  Its only job is
    // to make "answered *this* session" different from "replayed an earlier
    // handshake"; the startup secret is what proves the peer read the channel.
    spec.session_nonce = session_generation ^ 0x5EED'0000'0000'0000ull;
    spec.startup_secret = secret.value();

    auto launched = m5::launch_private_worker(context, worker_executable_, spec,
                                              processes_.job_handle());
    if (!launched.ok()) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            launched.error());
    }
    auto worker = launched.move_value();

    // The handle is *retained* before the session is built, and that order is the
    // one that cannot leak.  If building the session fails, the registry already
    // holds the worker, so the sweep will still find it and the next start will
    // clean it up; closing the handle here instead would leave a process that is
    // in the job and unreachable.
    auto retained = processes_.retain_launched(
        worker.binding.process_id, worker.binding.process_creation_time,
        worker.process_handle);
    if (!retained.ok()) {
        // Retention failed, so nothing owns the handle.  Close it rather than
        // dropping it, and report: a worker the registry refused is a worker no
        // sweep could account for, which is worse than a launch that failed.
        m5::close_launched_worker(worker);
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            retained.error());
    }
    // Ownership has moved into the registry.  Cleared so `close_launched_worker`
    // is a no-op from here on -- the registry needs the handle to wait for the
    // process, and closing it would leave a worker that can be seen and never
    // waited for.
    worker.process_handle = nullptr;

    // The fingerprint, recorded while both halves of it are in hand.
    //
    // This is the only place that knows the profile *and* the process together,
    // which is why it is here rather than in the sweep: a sweep runs in a later
    // process and has only the durable record to go on.  A failure is deliberately
    // **not** fatal to the launch.  The worker is running and useful; refusing it
    // because a hint could not be written would stop a disk over a bookkeeping
    // problem, and the cost of a missing fingerprint is bounded and visible -- the
    // next start reports that profile as needing attention instead of cleaning it
    // up.
    if (bindings_ != nullptr) {
        WorkerFingerprintRecord fingerprint{request.profile_id,
                                            worker.binding.process_id,
                                            worker.binding.process_creation_time,
                                            session_generation};
        static_cast<void>(bindings_->record(fingerprint));
    }

    auto session = ServiceWorkerSession::create(std::move(worker.channel),
                                                worker.binding);
    if (!session.ok()) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            session.error());
    }
    return Result<std::unique_ptr<ServiceWorkerSession>>::success(
        session.move_value());
}

Result<std::unique_ptr<ServiceWorkerSession>>
WindowsWorkerSessionProvider::attach(const product::Uuid&,
                                     const OperationContext&) {
    // Adopting a worker this provider did not launch needs a registry of live
    // workers, which belongs to the service composition rather than here -- the
    // adapter holds its own sessions and this provider owns none.  A successful
    // `nullopt` is the honest answer until that registry exists (Task 7), and
    // the adapter turns it into an explicit `execution_not_bound` refusal rather
    // than pretending a worker was found.
    return Result<std::unique_ptr<ServiceWorkerSession>>::success(nullptr);
}

} // namespace myrd::management
