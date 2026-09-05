#include "src/host/host_ipc.h"
#include "src/control/pipe_transport.h"
#include "src/host/host_core.h"
#include "src/host/volume_flush.h"
#include "src/host/volume_identity.h"
#include "src/m3/m3_windows_acceptance.h"
#include "src/persistence/image_inspector.h"
#include "src/persistence/vhdx_reader.h"
#include "src/profile/profile_store.h"
#include "src/profile/profile_validation.h"
#include "src/product/product_version.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::uint64_t file_time_value(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U)
        | value.dwLowDateTime;
}

myrd::product::Uuid fixed_uuid(const char* text) {
    auto value = myrd::product::Uuid::parse(text);
    if (!value.ok()) std::terminate();
    return value.move_value();
}

enum class ProductHostError : std::uint32_t {
    platform = 0x5c01,
    profile,
    binding,
    image,
    runtime,
    device,
    pipe,
    request,
};

template <class T>
myrd::Result<T> product_host_error(
    const ProductHostError code,
    const wchar_t* operation,
    const wchar_t* message,
    const std::uint32_t native_code = 0) {
    return myrd::Result<T>::failure({
        native_code == 0 ? myrd::ErrorDomain::internal
                         : myrd::ErrorDomain::win32,
        static_cast<std::uint32_t>(code),
        operation,
        message,
        native_code});
}

myrd::Result<void> remove_unaccepted_runtime_metadata(
    const std::wstring& program_data,
    const myrd::product::Uuid& profile_id) {
    auto directory =
        myrd::profile::ProfileDirectoryCapability::open_existing(
            program_data, profile_id);
    if (!directory.ok()) {
        return myrd::Result<void>::failure(directory.error());
    }
    constexpr std::array<std::wstring_view, 3> leaves{
        myrd::host::kRuntimeLeaseFileName,
        myrd::host::kRuntimeIdentityFileName,
        myrd::host::kRuntimeIdentityPreviousFileName};
    for (const auto leaf : leaves) {
        auto removed = directory.value().remove_file(leaf);
        if (!removed.ok()
            && removed.error().code != static_cast<std::uint32_t>(
                myrd::profile::ProfileStoreError::path_not_found)
            && removed.error().native_code != ERROR_FILE_NOT_FOUND
            && removed.error().native_code != ERROR_PATH_NOT_FOUND) {
            return removed;
        }
    }
    return directory.value().flush_directory();
}

bool uuid_is_zero(const myrd::product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](const std::byte byte) { return byte == std::byte{}; });
}

myrd::Result<std::wstring> trusted_program_data_root() {
    PWSTR raw = nullptr;
    const HRESULT status = SHGetKnownFolderPath(
        FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(status) || raw == nullptr) {
        return product_host_error<std::wstring>(
            ProductHostError::platform,
            L"host_main.program_data",
            L"The trusted ProgramData root could not be resolved",
            static_cast<std::uint32_t>(status));
    }
    struct KnownFolderOwner final {
        PWSTR value{};
        ~KnownFolderOwner() noexcept { CoTaskMemFree(value); }
    } owner{raw};
    try {
        std::filesystem::path root(raw);
        root = root.lexically_normal();
        if (!root.is_absolute()) {
            return product_host_error<std::wstring>(
                ProductHostError::platform,
                L"host_main.program_data",
                L"ProgramData did not resolve to an absolute path");
        }
        return myrd::Result<std::wstring>::success(root.wstring());
    } catch (...) {
        return product_host_error<std::wstring>(
            ProductHostError::platform,
            L"host_main.program_data",
            L"ProgramData path allocation failed");
    }
}

myrd::DynamicBlockStoreOptions store_options(
    const myrd::profile::Profile& profile) noexcept {
    auto options = myrd::m3::production_store_options();
    options.soft_limit_bytes = profile.memory.soft_limit_bytes;
    options.hard_limit_bytes = profile.memory.hard_limit_bytes;
    options.eviction_low_watermark_bytes =
        profile.memory.low_watermark_bytes;
    options.snapshot_cow_max_bytes =
        profile.memory.snapshot_cow_limit_bytes;
    return options;
}

myrd::m3::M3AimProviderFactoryConfig runtime_config(
    const myrd::profile::ProfileRecord& record,
    const myrd::OperationContext& context) {
    myrd::m3::M3AimProviderFactoryConfig config;
    config.context = context;
    config.provider.bundle_root = record.profile.bundle_path;
    config.provider.manifest_name = record.profile.manifest_name;
    config.provider.recovery.limits =
        myrd::persistence::production_persistence_limits(
        record.profile.logical_capacity_bytes);
    config.provider.store = store_options(record.profile);
    config.provider.request_timeout = std::chrono::seconds(30);
    return config;
}

myrd::host::HostCheckpointOutcome failed_checkpoint(
    myrd::Error error) {
    myrd::host::HostCheckpointOutcome result;
    result.error = std::move(error);
    return result;
}

myrd::host::HostStopOutcome failed_stop(myrd::Error error) {
    myrd::host::HostStopOutcome result;
    result.error = std::move(error);
    return result;
}

class AimHostProvider final : public myrd::host::IHostProvider {
public:
    AimHostProvider(
        myrd::profile::ProfileRecord record,
        std::uint64_t initial_generation) noexcept
        : record_(std::move(record)), generation_(initial_generation) {}

    myrd::Result<void> start(
        const myrd::OperationContext& context) override {
        std::scoped_lock lock(mutex_);
        if (state_ != myrd::host::HostProviderState::constructed) {
            return product_host_error<void>(
                ProductHostError::runtime,
                L"host_main.runtime_start",
                L"The AIM runtime was already started");
        }
        state_ = myrd::host::HostProviderState::starting;
        auto created = myrd::m3::M3AimRuntime::create(
            runtime_config(record_, context));
        if (!created.ok()) {
            state_ = myrd::host::HostProviderState::failed;
            return myrd::Result<void>::failure(created.error());
        }
        runtime_ = created.move_value();
        auto started = runtime_->start();
        if (!started.ok()) {
            state_ = myrd::host::HostProviderState::failed;
            return started;
        }
        state_ = myrd::host::HostProviderState::ready;
        return myrd::Result<void>::success();
    }

    myrd::host::HostCheckpointOutcome checkpoint(
        const myrd::OperationContext& context,
        const myrd::host::HostVolumeBarrierKind barrier) override {
        std::scoped_lock lock(mutex_);
        if (!runtime_ || state_ != myrd::host::HostProviderState::ready) {
            return failed_checkpoint(product_host_error<void>(
                ProductHostError::runtime,
                L"host_main.runtime_checkpoint",
                L"The AIM runtime is not ready").error());
        }
        auto mapped_barrier = myrd::host::m3_adapter::map_barrier(barrier);
        if (!mapped_barrier.ok()) {
            return failed_checkpoint(mapped_barrier.error());
        }
        auto mapped = myrd::host::m3_adapter::map_checkpoint_outcome(
            runtime_->checkpoint(context, mapped_barrier.value()));
        if (!mapped.ok()) return failed_checkpoint(mapped.error());
        if (mapped.value().ok()) generation_ = mapped.value().generation;
        return mapped.move_value();
    }

    myrd::host::HostStopOutcome stop(
        const myrd::OperationContext&,
        const myrd::host::HostStopRequest request) override {
        std::scoped_lock lock(mutex_);
        if (request.checkpoint) {
            return failed_stop(product_host_error<void>(
                ProductHostError::request,
                L"host_main.runtime_stop",
                L"Checkpointing stop must use the verified volume barrier")
                .error());
        }
        if (!runtime_
            || (state_ != myrd::host::HostProviderState::ready
                && state_ != myrd::host::HostProviderState::failed
                && state_ != myrd::host::HostProviderState::starting)) {
            return failed_stop(product_host_error<void>(
                ProductHostError::runtime,
                L"host_main.runtime_stop",
                L"The AIM runtime is not ready for stop").error());
        }
        state_ = myrd::host::HostProviderState::stopping;
        auto stopped = runtime_->stop();
        if (!stopped.ok()) {
            state_ = myrd::host::HostProviderState::failed;
            return failed_stop(stopped.error());
        }
        state_ = myrd::host::HostProviderState::stopped;
        myrd::host::HostStopOutcome result;
        result.generation = generation_;
        result.transport_stopped = true;
        result.store_closed = true;
        result.session_closed = true;
        result.writer_closed = true;
        return result;
    }

    myrd::Result<myrd::host::HostProviderState> state() const override {
        std::scoped_lock lock(mutex_);
        return myrd::Result<myrd::host::HostProviderState>::success(state_);
    }

    myrd::Result<std::uint64_t> generation() const override {
        std::scoped_lock lock(mutex_);
        return myrd::Result<std::uint64_t>::success(generation_);
    }

    myrd::Result<myrd::host::HostGeometry> geometry() const override {
        return myrd::Result<myrd::host::HostGeometry>::success({
            record_.profile.logical_capacity_bytes,
            record_.profile.logical_sector_size,
            record_.profile.physical_sector_size});
    }

    myrd::Result<myrd::host::HostStorageStats> storage_stats() const override {
        std::scoped_lock lock(mutex_);
        if (!runtime_) {
            return product_host_error<myrd::host::HostStorageStats>(
                ProductHostError::runtime,
                L"host_main.runtime_stats",
                L"The AIM runtime is absent");
        }
        return myrd::host::m3_adapter::map_storage_stats(
            runtime_->storage_stats());
    }

    myrd::Result<myrd::m3::M3AimRuntimeInfo> runtime_info() const {
        std::scoped_lock lock(mutex_);
        if (!runtime_ || state_ != myrd::host::HostProviderState::ready) {
            return product_host_error<myrd::m3::M3AimRuntimeInfo>(
                ProductHostError::runtime,
                L"host_main.runtime_info",
                L"The AIM runtime identity is unavailable");
        }
        return myrd::Result<myrd::m3::M3AimRuntimeInfo>::success(
            runtime_->info());
    }

    myrd::Result<myrd::proxy::RequestCounters> request_counters() const {
        std::scoped_lock lock(mutex_);
        if (!runtime_) {
            return product_host_error<myrd::proxy::RequestCounters>(
                ProductHostError::runtime,
                L"host_main.runtime_requests",
                L"The AIM runtime request counters are unavailable");
        }
        return myrd::Result<myrd::proxy::RequestCounters>::success(
            runtime_->transport_snapshot().requests);
    }

private:
    myrd::profile::ProfileRecord record_;
    mutable std::mutex mutex_;
    std::unique_ptr<myrd::m3::M3AimRuntime> runtime_;
    myrd::host::HostProviderState state_{
        myrd::host::HostProviderState::constructed};
    std::uint64_t generation_{};
};

class AimHostProviderFactory final : public myrd::host::IHostProviderFactory {
public:
    AimHostProviderFactory(
        myrd::profile::ProfileRecord record,
        const std::uint64_t initial_generation) noexcept
        : record_(std::move(record)), initial_generation_(initial_generation) {}

    myrd::Result<std::shared_ptr<myrd::host::IHostProvider>> open(
        const myrd::OperationContext&) override {
        try {
            auto provider = std::make_shared<AimHostProvider>(
                record_, initial_generation_);
            provider_ = provider;
            return myrd::Result<
                std::shared_ptr<myrd::host::IHostProvider>>::success(
                std::move(provider));
        } catch (...) {
            return product_host_error<
                std::shared_ptr<myrd::host::IHostProvider>>(
                ProductHostError::runtime,
                L"host_main.runtime_factory",
                L"The AIM host provider could not be allocated");
        }
    }

    std::shared_ptr<AimHostProvider> provider() const noexcept {
        return provider_.lock();
    }

private:
    myrd::profile::ProfileRecord record_;
    std::uint64_t initial_generation_{};
    std::weak_ptr<AimHostProvider> provider_;
};

myrd::Result<std::uint32_t> query_scsi_port(
    const std::wstring& physical_drive_path) {
    HANDLE raw = CreateFileW(
        physical_drive_path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return product_host_error<std::uint32_t>(
            ProductHostError::device,
            L"host_main.open_physical_drive",
            L"The exact physical drive could not be opened",
            GetLastError());
    }
    struct HandleOwner final {
        HANDLE value{};
        ~HandleOwner() noexcept {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        }
    } owner{raw};
    SCSI_ADDRESS address{};
    address.Length = sizeof(address);
    DWORD returned = 0;
    if (!DeviceIoControl(
            raw, IOCTL_SCSI_GET_ADDRESS, nullptr, 0,
            &address, sizeof(address), &returned, nullptr)
        || returned < sizeof(address)) {
        return product_host_error<std::uint32_t>(
            ProductHostError::device,
            L"host_main.query_scsi_address",
            L"The exact physical drive SCSI address could not be queried",
            GetLastError());
    }
    return myrd::Result<std::uint32_t>::success(address.PortNumber);
}

myrd::Result<myrd::profile::ProfileRecord> load_bound_profile(
    const std::wstring& program_data,
    const myrd::host::WorkerBinding& binding) {
    auto store = myrd::profile::ProfileStore::open_existing(
        program_data, binding.profile_id);
    if (!store.ok()) {
        return myrd::Result<myrd::profile::ProfileRecord>::failure(
            store.error());
    }
    auto record = store.value().load_current();
    if (!record.ok()) return record;
    const auto validation = myrd::profile::validate_profile(record.value());
    const auto& profile = record.value().profile;
    if (!validation.ok() || record.value().revision != binding.profile_revision
        || profile.profile_uuid != binding.profile_id
        || profile.image_uuid != binding.image_id
        || profile.lifecycle != myrd::profile::ProfileLifecycle::ready
        || !profile.gpt_disk_guid.has_value()
        || !profile.partition_guid.has_value()
        || !profile.volume_guid.has_value()) {
        return product_host_error<myrd::profile::ProfileRecord>(
            ProductHostError::binding,
            L"host_main.profile_binding",
            L"The durable profile does not match the worker startup binding");
    }
    return record;
}

myrd::Result<std::uint64_t> inspect_bound_image(
    const myrd::profile::ProfileRecord& record,
    const myrd::OperationContext& context) {
    myrd::persistence::ImageInspectOptions options;
    options.manifest_relative_name = record.profile.manifest_name;
    options.recovery.limits =
        myrd::persistence::production_persistence_limits(
        record.profile.logical_capacity_bytes);
    auto inspected = myrd::persistence::inspect_image(
        context, record.profile.bundle_path, options);
    if (!inspected.ok()) {
        return myrd::Result<std::uint64_t>::failure(inspected.error());
    }
    if (!inspected.value().selected.has_value()) {
        return product_host_error<std::uint64_t>(
            ProductHostError::image,
            L"host_main.image_binding",
            L"The bundle has no complete recoverable generation");
    }
    const auto& selected = inspected.value().selected.value();
    const bool image_matches = std::equal(
        record.profile.image_uuid.bytes().begin(),
        record.profile.image_uuid.bytes().end(),
        selected.image_uuid.bytes.begin(), selected.image_uuid.bytes.end());
    if (!image_matches
        || selected.geometry.virtual_bytes
            != record.profile.logical_capacity_bytes
        || selected.geometry.logical_sector_size
            != record.profile.logical_sector_size
        || selected.geometry.physical_sector_size
            != record.profile.physical_sector_size) {
        return product_host_error<std::uint64_t>(
            ProductHostError::image,
            L"host_main.image_binding",
            L"The selected image identity or geometry differs from the profile");
    }
    return myrd::Result<std::uint64_t>::success(
        selected.checkpoint_generation);
}

class ProductHost final {
public:
    static myrd::Result<std::unique_ptr<ProductHost>> create(
        myrd::host::StartupSecretMaterial material) {
        auto program_data = trusted_program_data_root();
        if (!program_data.ok()) {
            return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                program_data.error());
        }
        auto record = load_bound_profile(
            program_data.value(), material.binding);
        if (!record.ok()) {
            return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                record.error());
        }
        const myrd::OperationContext startup_context{
            {}, std::chrono::steady_clock::now() + std::chrono::minutes(5)};
        auto generation = inspect_bound_image(
            record.value(), startup_context);
        if (!generation.ok()) {
            return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                generation.error());
        }
        try {
            auto factory = std::make_shared<AimHostProviderFactory>(
                record.value(), generation.value());
            auto core = myrd::host::HostCore::create(
                material.binding, factory);
            if (!core.ok()) {
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    core.error());
            }
            // Publish the process-bound lease before the first AIM/device
            // mutation.  If this worker or the service dies in any later
            // startup phase, the next manager sees a stale lease and enters
            // fail-closed recovery instead of blindly creating a duplicate
            // disk.  Clean controlled stop removes the lease only after exact
            // volume/device removal is confirmed.
            auto lease_directory = myrd::host::open_runtime_lease_directory(
                program_data.value(), material.binding.profile_id);
            if (!lease_directory.ok()) {
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    lease_directory.error());
            }
            auto published = myrd::host::publish_runtime_lease_atomically(
                *lease_directory.value(), material.binding, material.secret);
            if (!published.ok()) {
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    published.error());
            }
            auto operation_id = myrd::product::Uuid::random();
            if (!operation_id.ok()) {
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    operation_id.error());
            }
            const myrd::host::HostCommand start_command{
                material.binding, operation_id.move_value()};
            auto started = core.value()->start(
                start_command, startup_context);
            if (!started.ok()) {
                const bool cleanup_proven = !started.provider_opened
                    || (started.cleanup.has_value()
                        && started.cleanup->ok());
                if (cleanup_proven) {
                    auto removed = remove_unaccepted_runtime_metadata(
                        program_data.value(), material.binding.profile_id);
                    if (!removed.ok()) {
                        return myrd::Result<
                            std::unique_ptr<ProductHost>>::failure(
                                removed.error());
                    }
                }
                if (started.error.has_value()) {
                    return myrd::Result<
                        std::unique_ptr<ProductHost>>::failure(
                            started.error.value());
                }
                return product_host_error<std::unique_ptr<ProductHost>>(
                    ProductHostError::runtime,
                    L"host_main.start",
                    L"The bound M3/AIM runtime did not start");
            }
            auto provider = factory->provider();
            if (!provider) {
                return product_host_error<std::unique_ptr<ProductHost>>(
                    ProductHostError::runtime,
                    L"host_main.start",
                    L"The started AIM provider capability is absent");
            }
            auto app = std::unique_ptr<ProductHost>(new ProductHost(
                program_data.move_value(), record.move_value(),
                material.binding, std::move(core.value()),
                std::move(factory), std::move(provider),
                generation.value()));
            auto verified = app->verify_volume(startup_context);
            if (!verified.ok()) {
                static_cast<void>(
                    app->rollback_unaccepted_startup(startup_context));
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    verified.error());
            }
            app->verified_volume_ = verified.move_value();

            // Publish the complete device/checkpoint identity before opening
            // the manager endpoint.  The service replaces this file only
            // after authenticated status exchange; until then it is a
            // fail-closed crash-recovery anchor for this verified product
            // worker and exact startup binding.
            auto initial_status = app->runtime_status();
            if (!initial_status.ok()) {
                static_cast<void>(
                    app->rollback_unaccepted_startup(startup_context));
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    initial_status.error());
            }
            auto encoded_status =
                myrd::host::encode_worker_runtime_status(
                    initial_status.value());
            if (!encoded_status.ok()) {
                static_cast<void>(
                    app->rollback_unaccepted_startup(startup_context));
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    encoded_status.error());
            }
            auto profile_directory =
                myrd::profile::ProfileDirectoryCapability::open_existing(
                    app->program_data_, app->binding_.profile_id);
            if (!profile_directory.ok()) {
                static_cast<void>(
                    app->rollback_unaccepted_startup(startup_context));
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    profile_directory.error());
            }
            auto identity_published =
                myrd::profile::publish_atomically_with_previous(
                    profile_directory.value(),
                    myrd::host::kRuntimeIdentityFileName,
                    myrd::host::kRuntimeIdentityPreviousFileName,
                    encoded_status.value());
            if (!identity_published.ok()) {
                static_cast<void>(
                    app->rollback_unaccepted_startup(startup_context));
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    identity_published.error());
            }

            auto endpoint = myrd::host::WorkerReattachmentEndpoint::create(
                app->binding_, std::move(material.secret),
                [raw = app.get()](const myrd::host::HostMessage& request) {
                    return raw->handle_request(request);
                });
            if (!endpoint.ok()) {
                return myrd::Result<std::unique_ptr<ProductHost>>::failure(
                    endpoint.error());
            }
            app->endpoint_ = endpoint.move_value();
            return myrd::Result<std::unique_ptr<ProductHost>>::success(
                std::move(app));
        } catch (...) {
            return product_host_error<std::unique_ptr<ProductHost>>(
                ProductHostError::runtime,
                L"host_main.create",
                L"The product host composition could not be allocated");
        }
    }

    int run() noexcept {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            auto pipe = myrd::host::PrivatePipeInstance::create(
                binding_.profile_id);
            if (!pipe.ok()) return 40;
            auto served = serve_connection(pipe.move_value());
            if (!served.ok()
                && !stop_requested_.load(std::memory_order_acquire)) {
                // A malformed or abandoned manager connection is isolated to
                // that connection.  The runtime and device remain alive for
                // authenticated service reattachment.
                continue;
            }
        }
        return stop_confirmed_.load(std::memory_order_acquire) ? 0 : 41;
    }

private:
    myrd::Result<void> rollback_unaccepted_startup(
        const myrd::OperationContext& context) {
        auto operation_id = myrd::product::Uuid::random();
        if (!operation_id.ok()) {
            return myrd::Result<void>::failure(operation_id.error());
        }
        const myrd::host::HostCommand command{
            binding_, operation_id.move_value()};
        auto stopped = core_->stop(command, context, {false, std::nullopt});
        if (!stopped.ok()) {
            return myrd::Result<void>::failure(
                stopped.error.value_or(product_host_error<void>(
                    ProductHostError::runtime,
                    L"host_main.startup_rollback",
                    L"Startup rollback failed without an error").error()));
        }
        return remove_unaccepted_runtime_metadata(
            program_data_, binding_.profile_id);
    }

    ProductHost(
        std::wstring program_data,
        myrd::profile::ProfileRecord record,
        myrd::host::WorkerBinding binding,
        std::unique_ptr<myrd::host::HostCore> core,
        std::shared_ptr<AimHostProviderFactory> factory,
        std::shared_ptr<AimHostProvider> provider,
        const std::uint64_t generation) noexcept
        : program_data_(std::move(program_data)), record_(std::move(record)),
          binding_(std::move(binding)), core_(std::move(core)),
          factory_(std::move(factory)), provider_(std::move(provider)),
          generation_(generation),
          checkpoint_deadline_unix_ms_(
              unix_time_ms() + checkpoint_interval_ms()) {}

    static std::uint64_t unix_time_ms() noexcept {
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        constexpr std::uint64_t windows_to_unix_100ns =
            116444736000000000ULL;
        const auto windows = file_time_value(time);
        return windows >= windows_to_unix_100ns
            ? (windows - windows_to_unix_100ns) / 10'000ULL : 0;
    }

    std::uint64_t checkpoint_interval_ms() const noexcept {
        if (!record_.profile.checkpoint.interval_enabled) return 0;
        constexpr std::uint64_t max_seconds =
            (std::numeric_limits<std::uint64_t>::max)() / 1'000ULL;
        return std::min(
            record_.profile.checkpoint.interval_seconds, max_seconds)
            * 1'000ULL;
    }

    myrd::Result<myrd::host::VerifiedVolumeIdentity> verify_volume(
        const myrd::OperationContext& context) {
        auto runtime = provider_->runtime_info();
        if (!runtime.ok()) {
            return myrd::Result<
                myrd::host::VerifiedVolumeIdentity>::failure(
                runtime.error());
        }
        auto port = query_scsi_port(runtime.value().disk.path);
        if (!port.ok()) {
            return myrd::Result<
                myrd::host::VerifiedVolumeIdentity>::failure(port.error());
        }
        const auto current_binding = endpoint_
            ? endpoint_->current_binding() : binding_;
        const auto address = myrd::aim::decode_device_address(
            runtime.value().device_id);
        myrd::host::VolumeIdentityExpectation expected{
            {current_binding.profile_id,
             current_binding.image_id,
             current_binding.profile_revision,
             current_binding.session_generation,
             runtime.value().device_id},
            {runtime.value().disk.physical_drive_number,
             runtime.value().disk.path,
             runtime.value().disk.length_bytes,
             port.value(),
             address.path_id,
             address.target_id,
             address.lun},
            record_.profile.gpt_disk_guid.value(),
            record_.profile.partition_guid.value(),
            record_.profile.volume_guid.value(),
            std::nullopt};
        myrd::Error last = product_host_error<void>(
            ProductHostError::device,
            L"host_main.verify_volume",
            L"The exact volume was not observed").error();
        do {
            auto verified = myrd::host::verify_windows_volume_identity(
                expected);
            if (verified.ok()) return verified;
            last = verified.error();
            if (std::chrono::steady_clock::now() >= context.deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        } while (std::chrono::steady_clock::now() < context.deadline);
        return myrd::Result<myrd::host::VerifiedVolumeIdentity>::failure(
            std::move(last));
    }

    myrd::Result<myrd::host::WorkerRuntimeStatus> runtime_status() {
        auto info = provider_->runtime_info();
        auto stats = provider_->storage_stats();
        auto requests = provider_->request_counters();
        if (!info.ok()) {
            return myrd::Result<myrd::host::WorkerRuntimeStatus>::failure(
                info.error());
        }
        if (!stats.ok()) {
            return myrd::Result<myrd::host::WorkerRuntimeStatus>::failure(
                stats.error());
        }
        if (!requests.ok()) {
            return myrd::Result<myrd::host::WorkerRuntimeStatus>::failure(
                requests.error());
        }
        if (!verified_volume_.has_value()
            || requests.value().write_errors > requests.value().write
            || requests.value().unmap_errors > requests.value().unmap
            || requests.value().zero_errors > requests.value().zero) {
            return product_host_error<myrd::host::WorkerRuntimeStatus>(
                ProductHostError::runtime,
                L"host_main.runtime_status",
                L"The runtime identity or proxy counters are contradictory");
        }
        const auto mutations =
            (requests.value().write - requests.value().write_errors)
            + (requests.value().unmap - requests.value().unmap_errors)
            + (requests.value().zero - requests.value().zero_errors);
        if (mutations != observed_mutations_) {
            observed_mutations_ = mutations;
            ++dirty_epoch_;
        }
        const bool dirty = stats.value().resident_dirty_blocks != 0
            || stats.value().zero_dirty_blocks != 0;
        const auto current_binding = endpoint_
            ? endpoint_->current_binding() : binding_;
        const auto address = myrd::aim::decode_device_address(
            info.value().device_id);
        constexpr std::uint64_t heartbeat_interval_ms = 2'000;
        const auto heartbeat = std::max<std::uint64_t>(
            1, GetTickCount64() / heartbeat_interval_ms);
        ++snapshot_sequence_;
        return myrd::Result<myrd::host::WorkerRuntimeStatus>::success({
            current_binding,
            {info.value().disk.physical_drive_number,
             info.value().disk.length_bytes,
             verified_volume_->pinned_expectation().physical_disk.port_number,
             address.path_id,
             address.target_id,
             address.lun,
             record_.profile.gpt_disk_guid.value(),
             record_.profile.partition_guid.value(),
             record_.profile.volume_guid.value()},
            info.value().device_id.value,
            generation_,
            info.value().native_object_path,
            heartbeat,
            snapshot_sequence_,
            dirty_epoch_,
            dirty,
            record_.profile.checkpoint.interval_enabled,
            current_binding.profile_revision,
            checkpoint_deadline_unix_ms_,
            std::nullopt,
            0,
            false});
    }

    myrd::Result<myrd::host::WorkerBinding> command_binding() {
        if (!endpoint_) {
            return product_host_error<myrd::host::WorkerBinding>(
                ProductHostError::runtime,
                L"host_main.command_binding",
                L"The authenticated worker endpoint is unavailable");
        }
        auto binding = endpoint_->current_binding();
        auto adopted = core_->adopt_reattached_binding(binding);
        if (!adopted.ok()) {
            return myrd::Result<myrd::host::WorkerBinding>::failure(
                adopted.error());
        }
        return myrd::Result<myrd::host::WorkerBinding>::success(
            std::move(binding));
    }

    myrd::Result<myrd::host::WorkerCheckpointReply> checkpoint(
        const std::uint32_t timeout_ms,
        const myrd::product::Uuid& operation_id) {
        if (timeout_ms == 0 || timeout_ms > 120'000) {
            return product_host_error<myrd::host::WorkerCheckpointReply>(
                ProductHostError::request,
                L"host_main.checkpoint",
                L"The checkpoint deadline budget is invalid");
        }
        std::scoped_lock operation_lock(operation_mutex_);
        const myrd::OperationContext context{
            {}, std::chrono::steady_clock::now()
                + std::chrono::milliseconds(timeout_ms)};
        auto binding = command_binding();
        if (!binding.ok()) {
            return myrd::Result<
                myrd::host::WorkerCheckpointReply>::failure(
                binding.error());
        }
        auto volume = verify_volume(context);
        if (!volume.ok()) {
            return myrd::Result<myrd::host::WorkerCheckpointReply>::failure(
                volume.error());
        }
        auto platform = myrd::host::make_windows_volume_flush_platform();
        if (!platform) {
            return product_host_error<myrd::host::WorkerCheckpointReply>(
                ProductHostError::runtime,
                L"host_main.checkpoint",
                L"The Windows volume flush platform is unavailable");
        }
        auto flushed = myrd::host::flush_volume_for_checkpoint(
            *platform, volume.value(), context);
        if (!flushed.ok()) {
            return myrd::Result<myrd::host::WorkerCheckpointReply>::failure(
                flushed.error.value_or(product_host_error<void>(
                    ProductHostError::device,
                    L"host_main.checkpoint",
                    L"The exact volume flush was not confirmed").error()));
        }
        const myrd::host::HostCommand command{
            binding.move_value(), operation_id};
        auto outcome = core_->checkpoint(
            command, context,
            myrd::host::HostVolumeBarrierKind::windows_volume_flushed);
        if (outcome.ok()) {
            generation_ = outcome.generation;
            const auto interval = checkpoint_interval_ms();
            checkpoint_deadline_unix_ms_ = interval == 0
                ? 0 : unix_time_ms() + interval;
        }
        return myrd::Result<myrd::host::WorkerCheckpointReply>::success({
            outcome.ok(), outcome.generation});
    }

    myrd::Result<myrd::host::WorkerControlledStopReply> controlled_stop(
        const std::uint32_t timeout_ms,
        const myrd::product::Uuid& operation_id) {
        if (timeout_ms == 0 || timeout_ms > 120'000) {
            return product_host_error<myrd::host::WorkerControlledStopReply>(
                ProductHostError::request,
                L"host_main.controlled_stop",
                L"The controlled-stop deadline budget is invalid");
        }
        std::scoped_lock operation_lock(operation_mutex_);
        const myrd::OperationContext context{
            {}, std::chrono::steady_clock::now()
                + std::chrono::milliseconds(timeout_ms)};
        auto binding = command_binding();
        if (!binding.ok()) {
            return myrd::Result<
                myrd::host::WorkerControlledStopReply>::failure(
                binding.error());
        }
        auto status = runtime_status();
        if (!status.ok()) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            std::wcerr << L"host-stop-error\tstatus\t"
                       << status.error().operation << L"\t"
                       << status.error().native_code << L"\t"
                       << status.error().message << std::endl;
#endif
            return myrd::Result<
                myrd::host::WorkerControlledStopReply>::failure(
                status.error());
        }
        if (record_.profile.checkpoint.normal_stop
                == myrd::profile::NormalStopPolicy::require_clean
            && status.value().dirty) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            std::wcerr << L"host-stop-error\tdirty-policy\t0\t"
                       << L"Profile requires an already-clean runtime"
                       << std::endl;
#endif
            return myrd::Result<
                myrd::host::WorkerControlledStopReply>::success(
                {false, false, generation_});
        }
        auto volume = verify_volume(context);
        if (!volume.ok()) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            std::wcerr << L"host-stop-error\tvolume\t"
                       << volume.error().operation << L"\t"
                       << volume.error().native_code << L"\t"
                       << volume.error().message << std::endl;
#endif
            return myrd::Result<
                myrd::host::WorkerControlledStopReply>::failure(
                volume.error());
        }
        auto platform = myrd::host::make_windows_volume_flush_platform();
        if (!platform) {
            return product_host_error<
                myrd::host::WorkerControlledStopReply>(
                ProductHostError::runtime,
                L"host_main.controlled_stop",
                L"The Windows volume flush platform is unavailable");
        }
        const myrd::host::HostCommand command{
            binding.move_value(), operation_id};
        auto checkpointed = myrd::host::checkpoint_volume_for_removal(
            *platform, *core_, command, volume.value(), context);
        if (!checkpointed.ok()) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            const auto* error = checkpointed.error.has_value()
                ? &checkpointed.error.value()
                : checkpointed.volume_flush.error.has_value()
                    ? &checkpointed.volume_flush.error.value()
                    : checkpointed.volume_flush.cleanup_error.has_value()
                        ? &checkpointed.volume_flush.cleanup_error.value()
                        : nullptr;
            std::wcerr << L"host-stop-error\tcheckpoint\t"
                       << (error ? error->operation : L"missing-error")
                       << L"\t" << (error ? error->native_code : 0)
                       << L"\t"
                       << (error ? error->message
                                 : L"Checkpoint failed without error evidence")
                       << std::endl;
#endif
            const auto generation = checkpointed.checkpoint.has_value()
                ? checkpointed.checkpoint->generation : generation_;
            return myrd::Result<
                myrd::host::WorkerControlledStopReply>::success(
                {false, false, generation});
        }
        auto removal = myrd::host::prepare_volume_for_removal(
            *platform, checkpointed.proof.value(), context);
        if (!removal.ok()) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            const auto* error = removal.error.has_value()
                ? &removal.error.value()
                : removal.cleanup_error.has_value()
                    ? &removal.cleanup_error.value() : nullptr;
            std::wcerr << L"host-stop-error\tremoval-barrier\t"
                       << (error ? error->operation : L"missing-error")
                       << L"\t" << (error ? error->native_code : 0)
                       << L"\t"
                       << (error ? error->message
                                 : L"Removal barrier failed without error evidence")
                       << std::endl;
#endif
            return myrd::Result<
                myrd::host::WorkerControlledStopReply>::success(
                {false, true,
                 checkpointed.proof->persistence_generation()});
        }
        auto stopped = core_->stop(
            command, context, {false, std::nullopt});
        if (!stopped.ok()) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            const auto* error = stopped.error.has_value()
                ? &stopped.error.value() : nullptr;
            std::wcerr << L"host-stop-error\truntime-stop\t"
                       << (error ? error->operation : L"missing-error")
                       << L"\t" << (error ? error->native_code : 0)
                       << L"\t"
                       << (error ? error->message
                                 : L"Runtime stop failed without error evidence")
                       << std::endl;
#endif
            return myrd::Result<
                myrd::host::WorkerControlledStopReply>::success(
                {false, true,
                 checkpointed.proof->persistence_generation()});
        }
        generation_ = checkpointed.proof->persistence_generation();
        stop_confirmed_.store(true, std::memory_order_release);
        stop_requested_.store(true, std::memory_order_release);
        return myrd::Result<
            myrd::host::WorkerControlledStopReply>::success(
            {true, true, generation_});
    }

    myrd::Result<myrd::host::HostMessage> handle_request(
        const myrd::host::HostMessage& request) {
        if (!request.payload.empty() || request.flags == 0
            || request.flags > 120'000) {
            if (request.command_id
                != myrd::host::kWorkerRuntimeSnapshotQuery) {
                return product_host_error<myrd::host::HostMessage>(
                    ProductHostError::request,
                    L"host_main.request",
                    L"Worker control request framing is invalid");
            }
        }
        if (request.command_id
            == myrd::host::kWorkerRuntimeSnapshotQuery) {
            if (request.flags != 0 || !request.payload.empty()) {
                return product_host_error<myrd::host::HostMessage>(
                    ProductHostError::request,
                    L"host_main.status",
                    L"Runtime status query must have an empty body");
            }
            auto status = runtime_status();
            if (!status.ok()) {
                return myrd::Result<myrd::host::HostMessage>::failure(
                    status.error());
            }
            auto payload = myrd::host::encode_worker_runtime_status(
                status.value());
            if (!payload.ok()) {
                return myrd::Result<myrd::host::HostMessage>::failure(
                    payload.error());
            }
            return myrd::Result<myrd::host::HostMessage>::success({
                request.request_id,
                request.operation_id,
                myrd::host::kWorkerRuntimeSnapshotResponse,
                0,
                payload.move_value()});
        }
        if (request.command_id == myrd::host::kWorkerCheckpointRequest) {
            auto reply = checkpoint(request.flags, request.operation_id);
            if (!reply.ok()) {
                return myrd::Result<myrd::host::HostMessage>::failure(
                    reply.error());
            }
            auto payload = myrd::host::encode_worker_checkpoint_reply(
                reply.value());
            if (!payload.ok()) {
                return myrd::Result<myrd::host::HostMessage>::failure(
                    payload.error());
            }
            return myrd::Result<myrd::host::HostMessage>::success({
                request.request_id,
                request.operation_id,
                myrd::host::kWorkerCheckpointResponse,
                0,
                payload.move_value()});
        }
        if (request.command_id
            == myrd::host::kWorkerControlledStopRequest) {
            auto reply = controlled_stop(request.flags, request.operation_id);
            if (!reply.ok()) {
                return myrd::Result<myrd::host::HostMessage>::failure(
                    reply.error());
            }
            auto payload = myrd::host::encode_worker_controlled_stop_reply(
                reply.value());
            if (!payload.ok()) {
                return myrd::Result<myrd::host::HostMessage>::failure(
                    payload.error());
            }
            return myrd::Result<myrd::host::HostMessage>::success({
                request.request_id,
                request.operation_id,
                myrd::host::kWorkerControlledStopResponse,
                0,
                payload.move_value()});
        }
        return product_host_error<myrd::host::HostMessage>(
            ProductHostError::request,
            L"host_main.request",
            L"The authenticated worker command is unsupported");
    }

    myrd::Result<void> serve_connection(
        myrd::host::PrivatePipeInstance pipe) {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            return product_host_error<void>(
                ProductHostError::pipe,
                L"host_main.pipe_event",
                L"The private-pipe connect event could not be created",
                GetLastError());
        }
        struct EventOwner final {
            HANDLE value{};
            ~EventOwner() noexcept { if (value != nullptr) CloseHandle(value); }
        } event_owner{event};
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        bool already_connected = false;
        BOOL connected = ConnectNamedPipe(
            static_cast<HANDLE>(pipe.native_handle()), &overlapped);
        if (!connected) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                already_connected = true;
                SetEvent(event);
            } else if (error != ERROR_IO_PENDING) {
                return product_host_error<void>(
                    ProductHostError::pipe,
                    L"host_main.pipe_connect",
                    L"The private-pipe connection could not be started",
                    error);
            }
        }
        const DWORD waited = WaitForSingleObject(event, 60'000);
        if (waited != WAIT_OBJECT_0) {
            CancelIoEx(static_cast<HANDLE>(pipe.native_handle()), &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(
                static_cast<HANDLE>(pipe.native_handle()),
                &overlapped, &ignored, TRUE);
            return myrd::Result<void>::success();
        }
        DWORD transferred = 0;
        if (!already_connected && !GetOverlappedResult(
                static_cast<HANDLE>(pipe.native_handle()),
                &overlapped, &transferred, FALSE)
            && GetLastError() != ERROR_PIPE_CONNECTED) {
            return product_host_error<void>(
                ProductHostError::pipe,
                L"host_main.pipe_connect",
                L"The private-pipe connection did not complete",
                GetLastError());
        }
        auto transport = myrd::ipc::PipeTransport::adopt_connected_handle(
            pipe.release());
        if (!transport.ok()) {
            return myrd::Result<void>::failure(transport.error());
        }
        const auto next_context = [] {
            return myrd::OperationContext{
                {}, std::chrono::steady_clock::now()
                    + std::chrono::seconds(35)};
        };
        auto hello_frame = transport.value()->read_frame(next_context());
        if (!hello_frame.ok()
            || hello_frame.value().message_type
                != myrd::ipc::MessageType::request
            || hello_frame.value().command_id
                != myrd::host::kPrivateWireClientHello) {
            return product_host_error<void>(
                ProductHostError::pipe,
                L"host_main.pipe_hello",
                L"The private-pipe client hello is invalid");
        }
        auto hello = myrd::host::decode_client_hello(
            hello_frame.value().payload);
        if (!hello.ok()) return myrd::Result<void>::failure(hello.error());
        const auto connection_nonce = hello.value().service_nonce;
        auto challenge = endpoint_->begin_attachment(
            connection_nonce, hello.value());
        if (!challenge.ok()) {
            return myrd::Result<void>::failure(challenge.error());
        }
        struct AttachmentOwner final {
            myrd::host::WorkerReattachmentEndpoint* endpoint{};
            myrd::host::HandshakeNonce nonce{};
            ~AttachmentOwner() noexcept {
                if (endpoint != nullptr) endpoint->disconnect_attachment(nonce);
            }
        } attachment{endpoint_.get(), connection_nonce};
        auto challenge_payload = myrd::host::encode_worker_challenge(
            challenge.value());
        if (!challenge_payload.ok()) {
            return myrd::Result<void>::failure(challenge_payload.error());
        }
        myrd::ipc::Frame challenge_frame{
            myrd::ipc::kProtocolMajor,
            myrd::ipc::kProtocolMinor,
            myrd::ipc::MessageType::response,
            0,
            hello_frame.value().request_id,
            hello_frame.value().operation_id,
            myrd::host::kPrivateWireWorkerChallenge,
            challenge_payload.move_value()};
        auto written = transport.value()->write_frame(
            next_context(), challenge_frame);
        if (!written.ok()) return written;

        auto proof_frame = transport.value()->read_frame(next_context());
        if (!proof_frame.ok()
            || proof_frame.value().message_type
                != myrd::ipc::MessageType::request
            || proof_frame.value().command_id
                != myrd::host::kPrivateWireServiceProof) {
            return product_host_error<void>(
                ProductHostError::pipe,
                L"host_main.pipe_proof",
                L"The private-pipe service proof is invalid");
        }
        auto proof = myrd::host::decode_service_proof(
            proof_frame.value().payload);
        if (!proof.ok()) return myrd::Result<void>::failure(proof.error());
        auto authenticated = endpoint_->authenticate_attachment(
            connection_nonce, proof.value());
        if (!authenticated.ok()) return authenticated;
        myrd::ipc::Frame acknowledged{
            myrd::ipc::kProtocolMajor,
            myrd::ipc::kProtocolMinor,
            myrd::ipc::MessageType::response,
            0,
            proof_frame.value().request_id,
            proof_frame.value().operation_id,
            myrd::host::kPrivateWireAuthenticated,
            {}};
        written = transport.value()->write_frame(next_context(), acknowledged);
        if (!written.ok()) return written;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            auto request_frame = transport.value()->read_frame(next_context());
            if (!request_frame.ok()) return myrd::Result<void>::success();
            if (request_frame.value().message_type
                    != myrd::ipc::MessageType::request
                || request_frame.value().command_id
                    != myrd::host::kPrivateWireExchangeRequest) {
                return product_host_error<void>(
                    ProductHostError::pipe,
                    L"host_main.pipe_exchange",
                    L"The authenticated private-pipe frame is invalid");
            }
            auto request = myrd::host::decode_authenticated_host_frame(
                request_frame.value().payload);
            if (!request.ok()) {
                return myrd::Result<void>::failure(request.error());
            }
            auto response = endpoint_->exchange(
                connection_nonce, request.value());
            if (!response.ok()) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
                std::wcerr << L"host-request-error\t"
                           << response.error().operation << L"\t"
                           << response.error().native_code << L"\t"
                           << response.error().message << std::endl;
#endif
                return myrd::Result<void>::failure(response.error());
            }
            auto payload = myrd::host::encode_authenticated_host_frame(
                response.value());
            if (!payload.ok()) {
                return myrd::Result<void>::failure(payload.error());
            }
            myrd::ipc::Frame response_frame{
                myrd::ipc::kProtocolMajor,
                myrd::ipc::kProtocolMinor,
                myrd::ipc::MessageType::response,
                0,
                request_frame.value().request_id,
                request_frame.value().operation_id,
                myrd::host::kPrivateWireExchangeResponse,
                payload.move_value()};
            written = transport.value()->write_frame(
                next_context(), response_frame);
            if (!written.ok()) return written;
        }
        return myrd::Result<void>::success();
    }

    std::wstring program_data_;
    myrd::profile::ProfileRecord record_;
    myrd::host::WorkerBinding binding_;
    std::unique_ptr<myrd::host::HostCore> core_;
    std::shared_ptr<AimHostProviderFactory> factory_;
    std::shared_ptr<AimHostProvider> provider_;
    std::optional<myrd::host::VerifiedVolumeIdentity> verified_volume_;
    std::shared_ptr<myrd::host::WorkerReattachmentEndpoint> endpoint_;
    std::mutex operation_mutex_;
    std::atomic<bool> stop_requested_{};
    std::atomic<bool> stop_confirmed_{};
    std::uint64_t generation_{};
    std::uint64_t observed_mutations_{};
    std::uint64_t dirty_epoch_{};
    std::uint64_t snapshot_sequence_{};
    std::uint64_t checkpoint_deadline_unix_ms_{};
};

#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
int run_pipe_probe(myrd::host::StartupSecretMaterial material) {
    const myrd::host::WorkerRuntimeStatus runtime{
        material.binding,
        {9,
         8ULL << 30,
         3,
         0,
         2,
         0,
         fixed_uuid("33333333-3333-4333-8333-333333333333"),
         fixed_uuid("55555555-5555-4555-8555-555555555555"),
         fixed_uuid("66666666-6666-4666-8666-666666666666")},
        0x00020003U,
        1,
        L"\\BaseNamedObjects\\Global\\myrd-m1-0123456789abcdef0123456789abcdef",
        1,
        1,
        0,
        false,
        false,
        1,
        0,
        std::nullopt,
        0,
        false};
    auto endpoint = myrd::host::WorkerReattachmentEndpoint::create(
        material.binding,
        std::move(material.secret),
        [runtime](const myrd::host::HostMessage& request)
            -> myrd::Result<myrd::host::HostMessage> {
            if (request.command_id
                    != myrd::host::kWorkerRuntimeSnapshotQuery
                || !request.payload.empty()) {
                return myrd::Result<myrd::host::HostMessage>::failure({
                    myrd::ErrorDomain::protocol,
                    static_cast<std::uint32_t>(
                        myrd::host::HostProtocolError::invalid_argument),
                    L"host_main.pipe_probe",
                    L"Engineering worker received an unexpected request"});
            }
            auto payload = myrd::host::encode_worker_runtime_status(runtime);
            if (!payload.ok()) {
                return myrd::Result<myrd::host::HostMessage>::failure(
                    payload.error());
            }
            return myrd::Result<myrd::host::HostMessage>::success({
                request.request_id,
                request.operation_id,
                myrd::host::kWorkerRuntimeSnapshotResponse,
                0,
                payload.move_value()});
        });
    if (!endpoint.ok()) return 10;
    auto pipe = myrd::host::PrivatePipeInstance::create_engineering_current_user(
        material.binding.profile_id);
    if (!pipe.ok()) return 11;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) return 12;
    struct EventOwner final {
        HANDLE value{};
        ~EventOwner() noexcept {
            if (value != nullptr) static_cast<void>(CloseHandle(value));
        }
    } event_owner{event};
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    bool already_connected = false;
    BOOL connected = ConnectNamedPipe(
        static_cast<HANDLE>(pipe.value().native_handle()), &overlapped);
    if (!connected) {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            already_connected = true;
            static_cast<void>(SetEvent(event));
        } else if (error != ERROR_IO_PENDING) {
            return 13;
        }
    }
    if (WaitForSingleObject(event, 10'000) != WAIT_OBJECT_0) {
        static_cast<void>(CancelIoEx(
            static_cast<HANDLE>(pipe.value().native_handle()), &overlapped));
        DWORD ignored = 0;
        static_cast<void>(GetOverlappedResult(
            static_cast<HANDLE>(pipe.value().native_handle()),
            &overlapped, &ignored, TRUE));
        return 14;
    }
    DWORD transferred = 0;
    if (!already_connected && !GetOverlappedResult(
            static_cast<HANDLE>(pipe.value().native_handle()),
            &overlapped, &transferred, FALSE)
        && GetLastError() != ERROR_PIPE_CONNECTED) {
        return 15;
    }
    auto transport = myrd::ipc::PipeTransport::adopt_connected_handle(
        pipe.value().release());
    if (!transport.ok()) return 16;
    const myrd::OperationContext context{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(30)};

    auto hello_frame = transport.value()->read_frame(context);
    if (!hello_frame.ok()
        || hello_frame.value().command_id
            != myrd::host::kPrivateWireClientHello) {
        return 17;
    }
    auto hello = myrd::host::decode_client_hello(
        hello_frame.value().payload);
    if (!hello.ok()) return 18;
    auto challenge = endpoint.value()->begin_attachment(
        hello.value().service_nonce, hello.value());
    if (!challenge.ok()) return 19;
    auto challenge_payload = myrd::host::encode_worker_challenge(
        challenge.value());
    if (!challenge_payload.ok()) return 20;
    myrd::ipc::Frame challenge_frame{
        myrd::ipc::kProtocolMajor,
        myrd::ipc::kProtocolMinor,
        myrd::ipc::MessageType::response,
        0,
        hello_frame.value().request_id,
        hello_frame.value().operation_id,
        myrd::host::kPrivateWireWorkerChallenge,
        challenge_payload.move_value()};
    if (!transport.value()->write_frame(context, challenge_frame).ok()) {
        return 21;
    }

    auto proof_frame = transport.value()->read_frame(context);
    if (!proof_frame.ok()
        || proof_frame.value().command_id
            != myrd::host::kPrivateWireServiceProof) {
        endpoint.value()->disconnect_attachment(
            hello.value().service_nonce);
        return 22;
    }
    auto proof = myrd::host::decode_service_proof(
        proof_frame.value().payload);
    if (!proof.ok()
        || !endpoint.value()->authenticate_attachment(
                hello.value().service_nonce, proof.value()).ok()) {
        endpoint.value()->disconnect_attachment(
            hello.value().service_nonce);
        return 23;
    }
    myrd::ipc::Frame acknowledged{
        myrd::ipc::kProtocolMajor,
        myrd::ipc::kProtocolMinor,
        myrd::ipc::MessageType::response,
        0,
        proof_frame.value().request_id,
        proof_frame.value().operation_id,
        myrd::host::kPrivateWireAuthenticated,
        {}};
    if (!transport.value()->write_frame(context, acknowledged).ok()) {
        endpoint.value()->disconnect_attachment(
            hello.value().service_nonce);
        return 24;
    }
    auto exchange_frame = transport.value()->read_frame(context);
    if (!exchange_frame.ok()
        || exchange_frame.value().command_id
            != myrd::host::kPrivateWireExchangeRequest) {
        endpoint.value()->disconnect_attachment(
            hello.value().service_nonce);
        return 25;
    }
    auto authenticated_request =
        myrd::host::decode_authenticated_host_frame(
            exchange_frame.value().payload);
    if (!authenticated_request.ok()) {
        endpoint.value()->disconnect_attachment(
            hello.value().service_nonce);
        return 26;
    }
    auto authenticated_response = endpoint.value()->exchange(
        hello.value().service_nonce, authenticated_request.value());
    if (!authenticated_response.ok()) {
        endpoint.value()->disconnect_attachment(
            hello.value().service_nonce);
        return 27;
    }
    auto response_payload = myrd::host::encode_authenticated_host_frame(
        authenticated_response.value());
    if (!response_payload.ok()) {
        endpoint.value()->disconnect_attachment(
            hello.value().service_nonce);
        return 28;
    }
    myrd::ipc::Frame exchange_response{
        myrd::ipc::kProtocolMajor,
        myrd::ipc::kProtocolMinor,
        myrd::ipc::MessageType::response,
        0,
        exchange_frame.value().request_id,
        exchange_frame.value().operation_id,
        myrd::host::kPrivateWireExchangeResponse,
        response_payload.move_value()};
    if (!transport.value()->write_frame(context, exchange_response).ok()) {
        endpoint.value()->disconnect_attachment(
            hello.value().service_nonce);
        return 29;
    }
    auto closed = transport.value()->read_frame(context);
    endpoint.value()->disconnect_attachment(hello.value().service_nonce);
    return closed.ok() ? 30 : 0;
}
#endif

} // namespace

int wmain(int argc, wchar_t** argv) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    if (argc != 3 && argc != 4) return 2;
#else
    if (argc != 3) return 2;
#endif
    if (std::wstring_view(argv[1]) != L"--profile") return 2;
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    const bool engineering_once = argc == 4
        && std::wstring_view(argv[3]) == L"--engineering-once";
    const bool engineering_pipe_probe = argc == 4
        && std::wstring_view(argv[3]) == L"--engineering-pipe-probe";
    if (argc == 4 && !engineering_once && !engineering_pipe_probe) return 2;
#endif

    const std::wstring_view profile_wide(argv[2]);
    std::string profile_text;
    profile_text.reserve(profile_wide.size());
    for (const wchar_t value : profile_wide) {
        if (value < 0 || value > 0x7f) return 2;
        profile_text.push_back(static_cast<char>(value));
    }
    auto profile = myrd::product::Uuid::parse(profile_text);
    if (!profile.ok()) return 3;

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(
            GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 4;
    }
    const myrd::host::StartupBindingConstraints constraints{
        profile.value(),
        GetCurrentProcessId(),
        file_time_value(creation),
        myrd::product::kProductVersionMajor,
        myrd::product::kProductVersionMinor,
        myrd::product::kProductVersionPatch};
    auto material = myrd::host::consume_startup_material(
        GetStdHandle(STD_INPUT_HANDLE), constraints);
    if (!material.ok()) return 5;

#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    if (engineering_once) return 0;
    if (engineering_pipe_probe) {
        return run_pipe_probe(material.move_value());
    }
#endif

    auto product_host = ProductHost::create(material.move_value());
    if (!product_host.ok()) {
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
        std::wcerr << L"host-startup\t" << product_host.error().operation
                   << L"\t" << product_host.error().native_code << L"\t"
                   << product_host.error().message << std::endl;
#endif
        return 6;
    }
    const int exit_code = product_host.value()->run();
    return exit_code;
}
