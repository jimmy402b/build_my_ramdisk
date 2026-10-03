#include "src/management/windows_worker_sessions.h"

#include "src/host/m5_worker_private_pipe.h"

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

// The durable profile's image uuid is a product uuid and the launch spec carries
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

// The drive letter the worker mounts on.  `mount_target` is the durable record's
// own text ("R:" for a drive-letter mount), and the worker takes the letter, so
// a target that is not one is refused rather than silently truncated.
Result<wchar_t> drive_letter_of(const profile::Profile& record) {
    if (record.mount_kind != profile::MountKind::drive_letter) {
        return Result<wchar_t>::failure(provider_error(
            L"WindowsWorkerSessionProvider::launch",
            L"only a drive-letter mount can be launched in M5"));
    }
    if (record.mount_target.size() < 2 || record.mount_target[1] != L':') {
        return Result<wchar_t>::failure(provider_error(
            L"WindowsWorkerSessionProvider::launch",
            L"the durable mount target is not a drive letter"));
    }
    return Result<wchar_t>::success(record.mount_target[0]);
}

} // namespace

WindowsWorkerSessionProvider::WindowsWorkerSessionProvider(
    std::wstring worker_executable) noexcept
    : worker_executable_(std::move(worker_executable)) {}

Result<std::unique_ptr<ServiceWorkerSession>>
WindowsWorkerSessionProvider::launch(const profile::ProfileRecord& profile,
                                     const OperationContext& context) {
    if (worker_executable_.empty()) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            provider_error(L"WindowsWorkerSessionProvider::launch",
                           L"the worker executable is not configured"));
    }
    const auto& record = profile.profile;
    auto letter = drive_letter_of(record);
    if (!letter.ok()) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            letter.error());
    }

    const auto session_generation = next_session_generation_++;
    auto secret = m5::StartupSecret::generate();
    if (!secret.ok()) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            secret.error());
    }

    m5::WorkerLaunchSpec spec{record.profile_uuid, profile.revision};
    spec.bundle_path = record.bundle_path;
    spec.image_id = to_image_uuid(record.image_uuid);
    spec.logical_size_bytes = record.logical_capacity_bytes;
    spec.mount_target = record.mount_target;
    spec.session_generation = session_generation;
    spec.private_pipe_name =
        m5::private_pipe_name(record.profile_uuid, session_generation);
    // A value this session generates and the worker must echo.  Its only job is
    // to make "answered *this* session" different from "replayed an earlier
    // handshake"; the startup secret is what proves the peer read the channel.
    spec.session_nonce = session_generation ^ 0x5EED'0000'0000'0000ull;
    spec.startup_secret = secret.value();

    auto launched = m5::launch_private_worker(context, worker_executable_, spec);
    if (!launched.ok()) {
        return Result<std::unique_ptr<ServiceWorkerSession>>::failure(
            launched.error());
    }
    auto worker = launched.move_value();
    auto session = ServiceWorkerSession::create(std::move(worker.channel),
                                                worker.binding);
    // The process handle is closed either way.  Nothing in the M5 adapter
    // observes the process -- a removal is proven by the worker's own
    // authenticated answer rather than by waiting for it to die -- so retaining
    // the handle would be a resource held for no reader.  When the composition
    // gains a live-worker registry it will need one, and that is where it
    // belongs.
    m5::close_launched_worker(worker);
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
