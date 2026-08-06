#include "src/m3/m3_windows_acceptance.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace myrd::m3 {
namespace {

bool named_objects_absent(
    const proxy::ProxyObjectNames& names,
    Error& error) {
    struct Probe final {
        HANDLE handle;
        const wchar_t* operation;
    };
    const std::array<Probe, 5> probes{{
        {OpenMutexW(SYNCHRONIZE, FALSE, names.server.c_str()), L"OpenMutexW"},
        {OpenFileMappingW(FILE_MAP_READ, FALSE, names.mapping.c_str()),
         L"OpenFileMappingW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.request.c_str()), L"OpenEventW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.response.c_str()), L"OpenEventW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.cancel.c_str()), L"OpenEventW"},
    }};
    for (const auto& probe : probes) {
        if (probe.handle != nullptr) {
            CloseHandle(probe.handle);
            return false;
        }
        const auto code = static_cast<std::uint32_t>(GetLastError());
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_INVALID_NAME) {
            error = {ErrorDomain::cleanup, code,
                     probe.operation, L"failed to inspect named object"};
            return false;
        }
    }
    return true;
}

class M3AimProviderLifecycle final : public IM1ProviderLifecycle {
public:
    M3AimProviderLifecycle(
        std::unique_ptr<M3Provider> provider,
        OperationContext context)
        : provider_(std::move(provider)),
          context_(std::move(context)),
          names_(provider_->names()) {}

    Result<void> start() override {
        return provider_->start(context_);
    }

    Result<void> stop_and_release() override {
        const auto stopped = provider_->stop(context_, {});
        final_snapshot_ = provider_->transport_snapshot();
        if (!stopped.ok()) {
            return Result<void>::failure(stopped.error.value_or(Error{
                ErrorDomain::cleanup,
                static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
                L"M3AimProviderLifecycle::stop_and_release",
                L"M3 provider stop failed without an error"}));
        }
        return Result<void>::success();
    }

    std::wstring native_object_path() const override {
        return names_.driver_path;
    }

    proxy::SessionSnapshot snapshot() const override {
        return final_snapshot_.value_or(provider_->transport_snapshot());
    }

    Result<bool> named_objects_released() const override {
        Error error{};
        const bool absent = named_objects_absent(names_, error);
        if (!absent && !error.operation.empty()) {
            return Result<bool>::failure(std::move(error));
        }
        return Result<bool>::success(absent);
    }

private:
    std::unique_ptr<M3Provider> provider_;
    OperationContext context_;
    proxy::ProxyObjectNames names_;
    std::optional<proxy::SessionSnapshot> final_snapshot_;
};

} // namespace

M3AimRuntime::M3AimRuntime(
    M3AimProviderFactoryConfig config,
    const std::chrono::milliseconds device_timeout)
    : config_(std::move(config)),
      device_timeout_(device_timeout) {}

M3AimRuntime::~M3AimRuntime() noexcept {
    try {
        (void)stop();
    } catch (...) {
    }
}

Result<std::unique_ptr<M3AimRuntime>> M3AimRuntime::create(
    M3AimProviderFactoryConfig config,
    const std::chrono::milliseconds device_timeout) {
    if (device_timeout.count() <= 0) {
        return Result<std::unique_ptr<M3AimRuntime>>::failure({
            ErrorDomain::usage, 0, L"M3AimRuntime::create",
            L"device timeout must be positive"});
    }
    return Result<std::unique_ptr<M3AimRuntime>>::success(
        std::unique_ptr<M3AimRuntime>(new M3AimRuntime(
            std::move(config), device_timeout)));
}

Result<void> M3AimRuntime::start() {
    if (started_) {
        return Result<void>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"M3AimRuntime::start", L"runtime is already started"});
    }
    auto checked = check_operation_context(config_.context);
    if (!checked) return checked;

    auto opened = M3Provider::open(config_.context, config_.provider);
    if (!opened) return Result<void>::failure(opened.error());
    provider_ = opened.move_value();
    auto started = provider_->start(config_.context);
    if (!started) {
        (void)stop_provider();
        return started;
    }

    auto discovered = aim::discover_aim_adapter();
    if (!discovered) {
        (void)stop_provider();
        return Result<void>::failure(discovered.error());
    }
    adapter_handle_ = discovered.move_value();
    transport_ = std::make_unique<aim::WindowsSrbTransport>(
        adapter_handle_->handle.get());
    adapter_ = std::make_unique<aim::DirectAimAdapter>(*transport_);

    auto baseline = aim::snapshot_physical_drive_numbers();
    if (!baseline) {
        (void)stop_provider();
        return Result<void>::failure(baseline.error());
    }
    physical_baseline_ = baseline.move_value();

    const auto geometry = provider_->geometry();
    const auto names = provider_->names();
    const aim::ProxyDiskSpec spec{
        geometry.virtual_bytes,
        geometry.logical_sector_size,
        names.driver_path};
    auto created = adapter_->create_proxy_disk(spec);
    if (!created) {
        (void)stop_provider();
        return Result<void>::failure(created.error());
    }
    if (created.value().value == aim::kAllDevices ||
        created.value().value == aim::kAutoDeviceNumber) {
        (void)stop_provider();
        return Result<void>::failure({
            ErrorDomain::evidence, 0, L"M3AimRuntime::start",
            L"driver returned an unsafe sentinel DeviceId"});
    }
    owned_device_ = created.value();

    auto rescanned = aim::rescan_aim_adapter(adapter_handle_->dev_inst);
    if (!rescanned) {
        (void)remove_owned_device();
        (void)stop_provider();
        return rescanned;
    }
    auto queried = adapter_->query_device(*owned_device_);
    if (!queried) {
        (void)remove_owned_device();
        (void)stop_provider();
        return Result<void>::failure(queried.error());
    }
    if (queried.value().id != *owned_device_ ||
        queried.value().size_bytes != geometry.virtual_bytes ||
        queried.value().sector_size != geometry.logical_sector_size ||
        queried.value().native_object_path != names.driver_path) {
        (void)remove_owned_device();
        (void)stop_provider();
        return Result<void>::failure({
            ErrorDomain::evidence, 0, L"M3AimRuntime::start",
            L"AIM QUERY_DEVICE identity did not match M3 provider"});
    }

    auto located = aim::wait_for_created_physical_drive(
        *owned_device_, *physical_baseline_, device_timeout_);
    if (!located) {
        (void)remove_owned_device();
        (void)stop_provider();
        return Result<void>::failure(located.error());
    }
    info_.device_id = *owned_device_;
    info_.disk = located.move_value();
    info_.virtual_bytes = geometry.virtual_bytes;
    info_.sector_size = geometry.logical_sector_size;
    started_ = true;
    return Result<void>::success();
}

M3CheckpointOutcome M3AimRuntime::checkpoint(
    const OperationContext& context,
    const persistence::VolumeBarrierKind barrier) {
    if (!started_ || !provider_) {
        return {persistence::CommitDisposition::NotCommitted, 0,
                Error{ErrorDomain::internal,
                      static_cast<std::uint32_t>(
                          OperationErrorCode::internal_invariant),
                      L"M3AimRuntime::checkpoint",
                      L"runtime is not started"}};
    }
    return provider_->checkpoint(context, barrier);
}

Result<void> M3AimRuntime::remove_owned_device() {
    if (!owned_device_ || !adapter_) {
        device_removed_ = true;
        physical_drive_restored_ = true;
        return Result<void>::success();
    }
    const auto deadline = std::chrono::steady_clock::now() + device_timeout_;
    if (!device_removed_) {
        auto removed = adapter_->remove_device(*owned_device_);
        if (!removed) return removed;
        for (;;) {
            auto devices = adapter_->list_devices();
            if (!devices) return Result<void>::failure(devices.error());
            const bool present = std::any_of(
                devices.value().begin(), devices.value().end(),
                [this](const aim::DeviceId value) {
                    return value == *owned_device_;
                });
            if (!present) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                return Result<void>::failure({
                    ErrorDomain::cleanup, WAIT_TIMEOUT,
                    L"M3AimRuntime::remove_owned_device",
                    L"owned AIM device remained present"});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // The AIM object is gone, but the PhysicalDrive may still be draining.
        // Keep the provider alive until that second cleanup boundary is met.
        device_removed_ = true;
    }

    if (physical_baseline_ && !physical_drive_restored_) {
        for (;;) {
            auto numbers = aim::snapshot_physical_drive_numbers();
            if (numbers && std::is_permutation(
                    numbers.value().begin(), numbers.value().end(),
                    physical_baseline_->begin(), physical_baseline_->end())) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return Result<void>::failure({
                    ErrorDomain::cleanup, WAIT_TIMEOUT,
                    L"M3AimRuntime::remove_owned_device",
                    L"PhysicalDrive inventory did not return to baseline"});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        physical_drive_restored_ = true;
    }
    return Result<void>::success();
}

Result<void> M3AimRuntime::stop_provider() {
    if (!provider_) return Result<void>::success();
    const auto stopped = provider_->stop(OperationContext{}, {});
    provider_.reset();
    adapter_.reset();
    transport_.reset();
    adapter_handle_.reset();
    if (!stopped.ok()) {
        return Result<void>::failure(stopped.error.value_or(Error{
            ErrorDomain::cleanup,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"M3AimRuntime::stop_provider",
            L"M3 provider stop failed without an error"}));
    }
    return Result<void>::success();
}

Result<void> M3AimRuntime::stop() {
    auto removed = remove_owned_device();
    if (!removed) {
        // Do not close the backend while the owned device or its PhysicalDrive
        // is still present. The caller/destructor may retry cleanup safely.
        return removed;
    }
    auto stopped = stop_provider();
    started_ = false;
    return stopped;
}

M3AimProviderFactory::M3AimProviderFactory(
    M3AimProviderFactoryConfig config)
    : config_(std::move(config)) {}

Result<std::unique_ptr<IM1ProviderLifecycle>>
M3AimProviderFactory::create() {
    auto checked = check_operation_context(config_.context);
    if (!checked) {
        return Result<std::unique_ptr<IM1ProviderLifecycle>>::failure(
            checked.error());
    }
    auto provider = M3Provider::open(config_.context, config_.provider);
    if (!provider) {
        return Result<std::unique_ptr<IM1ProviderLifecycle>>::failure(
            provider.error());
    }
    std::unique_ptr<IM1ProviderLifecycle> lifecycle =
        std::make_unique<M3AimProviderLifecycle>(
            provider.move_value(), config_.context);
    return Result<std::unique_ptr<IM1ProviderLifecycle>>::success(
        std::move(lifecycle));
}

} // namespace myrd::m3
