#include "tests/support/test_framework.h"

#include "src/host/volume_identity.h"
#include "src/host/volume_flush.h"
#include "src/product/product_version.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace {

using namespace myrd;
using namespace myrd::host;

std::optional<std::wstring> environment(const wchar_t* name) {
    std::size_t required = 0;
    if (_wgetenv_s(&required, nullptr, 0, name) != 0 || required == 0) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    std::size_t written = 0;
    if (_wgetenv_s(&written, value.data(), required, name) != 0
        || written != required) {
        return std::nullopt;
    }
    value.resize(written - 1);
    return value;
}

std::uint64_t parse_u64(const wchar_t* name) {
    const auto value = environment(name);
    REQUIRE(value.has_value());
    REQUIRE(!value->empty());
    REQUIRE(value->front() != L'-');
    wchar_t* end = nullptr;
    errno = 0;
    const auto parsed = std::wcstoull(value->c_str(), &end, 0);
    REQUIRE(errno == 0);
    REQUIRE(end != value->c_str());
    REQUIRE(*end == L'\0');
    return parsed;
}

product::Uuid parse_uuid(const wchar_t* name) {
    const auto wide = environment(name);
    REQUIRE(wide.has_value());
    std::string text;
    text.reserve(wide->size());
    for (const wchar_t value : *wide) {
        REQUIRE(value >= 0 && value <= 0x7F);
        const char byte = static_cast<char>(value);
        text.push_back(byte >= 'A' && byte <= 'F'
            ? static_cast<char>(byte - 'A' + 'a') : byte);
    }
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

product::Uuid fixed_uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

VolumeIdentityExpectation synthetic_expectation() {
    return {
        {fixed_uuid("00112233-4455-4677-8899-aabbccddeeff"),
         fixed_uuid("10213243-5465-4687-98a9-bacbdcedfe0f"),
         3,
         7,
         aim::DeviceId{0x00030201}},
        {42, L"\\\\.\\PhysicalDrive42", 8ULL << 20, 11, 1, 2, 3},
        fixed_uuid("20000000-0000-4000-8000-000000000001"),
        fixed_uuid("20000000-0000-4000-8000-000000000002"),
        fixed_uuid("20000000-0000-4000-8000-000000000003"),
        std::nullopt};
}

VolumeIdentityObservation synthetic_observation() {
    return {
        L"\\\\?\\Volume{20000000-0000-4000-8000-000000000003}\\",
        fixed_uuid("20000000-0000-4000-8000-000000000003"),
        0xA1B2C3D4,
        L"NTFS",
        kStorageDeviceTypeDisk,
        42,
        1,
        {{42, 1ULL << 20, 7ULL << 20}},
        {42, L"\\\\.\\PhysicalDrive42", 8ULL << 20, 11, 1, 2, 3},
        ObservedPartitionStyle::gpt,
        fixed_uuid("20000000-0000-4000-8000-000000000001"),
        1,
        fixed_uuid("20000000-0000-4000-8000-000000000002"),
        1ULL << 20,
        7ULL << 20};
}

VerifiedVolumeIdentity synthetic_verified() {
    const auto observed = synthetic_observation();
    const std::array observations{observed};
    auto verified = match_volume_identity(
        synthetic_expectation(), observations);
    REQUIRE(verified.ok());
    return verified.move_value();
}

WorkerBinding synthetic_binding() {
    const auto expected = synthetic_expectation();
    return {
        expected.owner.profile_uuid,
        expected.owner.image_uuid,
        42,
        100,
        expected.owner.session_generation,
        expected.owner.profile_revision,
        product::kProductVersionMajor,
        product::kProductVersionMinor,
        product::kProductVersionPatch};
}

HostCommand synthetic_command(std::string_view operation_id) {
    return {synthetic_binding(), fixed_uuid(operation_id)};
}

Error fake_error(
    VolumeFlushError code,
    const wchar_t* operation) {
    return {ErrorDomain::win32,
            static_cast<std::uint32_t>(code),
            operation,
            L"injected volume flush failure",
            170};
}

struct FlushProbe final {
    std::vector<std::string> events;
    VolumeIdentityObservation observation = synthetic_observation();
    bool mounted{true};
    std::optional<Error> open_error;
    std::optional<Error> observe_error;
    std::optional<Error> mounted_error;
    std::optional<Error> lock_error;
    std::optional<Error> flush_error;
    std::optional<Error> dismount_error;
    std::optional<Error> unlock_error;
    std::stop_source* cancel_during_lock{};
    std::stop_source* cancel_during_flush{};
};

class FakeVolumeHandle final : public IVolumeFlushHandle {
public:
    explicit FakeVolumeHandle(std::shared_ptr<FlushProbe> probe)
        : probe_(std::move(probe)) {}

    Result<VolumeIdentityObservation> observe(
        const OperationContext&) override {
        probe_->events.emplace_back("observe");
        if (probe_->observe_error.has_value()) {
            return Result<VolumeIdentityObservation>::failure(
                *probe_->observe_error);
        }
        return Result<VolumeIdentityObservation>::success(
            probe_->observation);
    }

    Result<bool> is_mounted(const OperationContext&) override {
        probe_->events.emplace_back("is_mounted");
        if (probe_->mounted_error.has_value()) {
            return Result<bool>::failure(*probe_->mounted_error);
        }
        return Result<bool>::success(probe_->mounted);
    }

    Result<void> lock(const OperationContext&) override {
        probe_->events.emplace_back("lock");
        if (probe_->cancel_during_lock != nullptr) {
            probe_->cancel_during_lock->request_stop();
        }
        if (probe_->lock_error.has_value()) {
            return Result<void>::failure(*probe_->lock_error);
        }
        return Result<void>::success();
    }

    Result<void> flush(const OperationContext&) override {
        probe_->events.emplace_back("flush");
        if (probe_->cancel_during_flush != nullptr) {
            probe_->cancel_during_flush->request_stop();
        }
        if (probe_->flush_error.has_value()) {
            return Result<void>::failure(*probe_->flush_error);
        }
        return Result<void>::success();
    }

    Result<void> dismount(const OperationContext&) override {
        probe_->events.emplace_back("dismount");
        if (probe_->dismount_error.has_value()) {
            return Result<void>::failure(*probe_->dismount_error);
        }
        return Result<void>::success();
    }

    Result<void> unlock(const OperationContext&) override {
        probe_->events.emplace_back("unlock");
        if (probe_->unlock_error.has_value()) {
            return Result<void>::failure(*probe_->unlock_error);
        }
        return Result<void>::success();
    }

private:
    std::shared_ptr<FlushProbe> probe_;
};

class FakeVolumePlatform final : public IVolumeFlushPlatform {
public:
    explicit FakeVolumePlatform(std::shared_ptr<FlushProbe> probe)
        : probe_(std::move(probe)) {}

    Result<std::unique_ptr<IVolumeFlushHandle>> open_exact(
        const VerifiedVolumeIdentity&,
        const OperationContext&) override {
        probe_->events.emplace_back("open");
        if (probe_->open_error.has_value()) {
            return Result<std::unique_ptr<IVolumeFlushHandle>>::failure(
                *probe_->open_error);
        }
        std::unique_ptr<IVolumeFlushHandle> handle =
            std::make_unique<FakeVolumeHandle>(probe_);
        return Result<std::unique_ptr<IVolumeFlushHandle>>::success(
            std::move(handle));
    }

private:
    std::shared_ptr<FlushProbe> probe_;
};

class FakeCheckpointProvider final : public IHostProvider {
public:
    Result<void> start(const OperationContext&) override {
        state_value = HostProviderState::ready;
        return Result<void>::success();
    }

    HostCheckpointOutcome checkpoint(
        const OperationContext&,
        HostVolumeBarrierKind barrier) override {
        ++checkpoint_calls;
        last_barrier = barrier;
        if (events != nullptr) events->emplace_back("m3_checkpoint");
        return checkpoint_outcome;
    }

    HostStopOutcome stop(
        const OperationContext&,
        HostStopRequest) override {
        state_value = HostProviderState::stopped;
        HostStopOutcome outcome{};
        outcome.transport_stopped = true;
        outcome.store_closed = true;
        outcome.session_closed = true;
        outcome.writer_closed = true;
        return outcome;
    }

    Result<HostProviderState> state() const override {
        return Result<HostProviderState>::success(state_value);
    }

    Result<std::uint64_t> generation() const override {
        return Result<std::uint64_t>::success(7);
    }

    Result<HostGeometry> geometry() const override {
        return Result<HostGeometry>::success({8ULL << 20, 512, 4096});
    }

    Result<HostStorageStats> storage_stats() const override {
        return Result<HostStorageStats>::success({});
    }

    HostCheckpointOutcome checkpoint_outcome{
        HostCommitDisposition::committed, 8};
    HostProviderState state_value{HostProviderState::constructed};
    std::optional<HostVolumeBarrierKind> last_barrier;
    std::vector<std::string>* events{};
    unsigned checkpoint_calls{};
};

class FakeCheckpointProviderFactory final : public IHostProviderFactory {
public:
    Result<std::shared_ptr<IHostProvider>> open(
        const OperationContext&) override {
        return Result<std::shared_ptr<IHostProvider>>::success(provider);
    }

    std::shared_ptr<FakeCheckpointProvider> provider =
        std::make_shared<FakeCheckpointProvider>();
};

struct CheckpointHarness final {
    std::shared_ptr<FakeCheckpointProviderFactory> factory;
    std::unique_ptr<HostCore> core;
};

CheckpointHarness started_checkpoint_host() {
    auto factory = std::make_shared<FakeCheckpointProviderFactory>();
    auto created = HostCore::create(synthetic_binding(), factory);
    REQUIRE(created.ok());
    auto core = created.move_value();
    const auto started = core->start(
        synthetic_command("30000000-0000-4000-8000-000000000001"), {});
    REQUIRE(started.ok());
    return {std::move(factory), std::move(core)};
}

SuccessfulCheckpoint successful_checkpoint() {
    auto probe = std::make_shared<FlushProbe>();
    FakeVolumePlatform platform(probe);
    auto host = started_checkpoint_host();
    auto outcome = checkpoint_volume_for_removal(
        platform,
        *host.core,
        synthetic_command("30000000-0000-4000-8000-000000000002"),
        synthetic_verified(),
        {});
    REQUIRE(outcome.ok());
    REQUIRE(outcome.proof.has_value());
    return *outcome.proof;
}

} // namespace

TEST_CASE(volume_checkpoint_flush_revalidates_and_never_locks_or_dismounts) {
    auto probe = std::make_shared<FlushProbe>();
    FakeVolumePlatform platform(probe);
    const auto outcome = flush_volume_for_checkpoint(
        platform, synthetic_verified(), {});
    REQUIRE(outcome.ok());
    REQUIRE(outcome.proof.has_value());
    REQUIRE_EQ(
        outcome.proof->purpose(),
        VolumeFlushPurpose::checkpoint_barrier);
    REQUIRE(outcome.flushed);
    REQUIRE(!outcome.locked);
    REQUIRE(!outcome.dismounted);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{"open", "observe", "flush"}));
}

TEST_CASE(volume_checkpoint_for_removal_orders_barrier_before_m3_commit) {
    auto probe = std::make_shared<FlushProbe>();
    FakeVolumePlatform platform(probe);
    auto host = started_checkpoint_host();
    host.factory->provider->events = &probe->events;

    const auto outcome = checkpoint_volume_for_removal(
        platform,
        *host.core,
        synthetic_command("30000000-0000-4000-8000-000000000003"),
        synthetic_verified(),
        {});

    REQUIRE(outcome.ok());
    REQUIRE(outcome.volume_flush.ok());
    REQUIRE(outcome.checkpoint.has_value());
    REQUIRE(outcome.checkpoint->ok());
    REQUIRE(outcome.proof.has_value());
    REQUIRE_EQ(outcome.proof->identity().owner(), synthetic_expectation().owner);
    REQUIRE_EQ(outcome.proof->persistence_generation(), 8ULL);
    REQUIRE_EQ(host.factory->provider->checkpoint_calls, 1U);
    REQUIRE(host.factory->provider->last_barrier.has_value());
    REQUIRE_EQ(
        *host.factory->provider->last_barrier,
        HostVolumeBarrierKind::windows_volume_flushed);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{
            "open", "observe", "flush", "m3_checkpoint"}));
}

TEST_CASE(volume_checkpoint_for_removal_never_authorizes_failed_m3_commit) {
    auto probe = std::make_shared<FlushProbe>();
    FakeVolumePlatform platform(probe);
    auto host = started_checkpoint_host();
    host.factory->provider->checkpoint_outcome = {
        HostCommitDisposition::not_committed,
        7,
        fake_error(VolumeFlushError::checkpoint_failed, L"M3.checkpoint"),
        std::nullopt};

    const auto outcome = checkpoint_volume_for_removal(
        platform,
        *host.core,
        synthetic_command("30000000-0000-4000-8000-000000000004"),
        synthetic_verified(),
        {});

    REQUIRE(!outcome.ok());
    REQUIRE(outcome.volume_flush.ok());
    REQUIRE(outcome.checkpoint.has_value());
    REQUIRE(!outcome.checkpoint->ok());
    REQUIRE(!outcome.proof.has_value());
}

TEST_CASE(volume_checkpoint_for_removal_rejects_all_non_committed_dispositions) {
    constexpr std::array dispositions{
        HostCommitDisposition::not_committed,
        HostCommitDisposition::commit_indeterminate,
        HostCommitDisposition::committed_needs_reconciliation};
    for (const auto disposition : dispositions) {
        auto probe = std::make_shared<FlushProbe>();
        FakeVolumePlatform platform(probe);
        auto host = started_checkpoint_host();
        host.factory->provider->checkpoint_outcome = {disposition, 7};

        const auto outcome = checkpoint_volume_for_removal(
            platform,
            *host.core,
            synthetic_command("30000000-0000-4000-8000-000000000006"),
            synthetic_verified(),
            {});

        REQUIRE(!outcome.ok());
        REQUIRE(outcome.checkpoint.has_value());
        REQUIRE(!outcome.checkpoint->ok());
        REQUIRE(!outcome.proof.has_value());
    }
}

TEST_CASE(volume_checkpoint_for_removal_checks_cancellation_between_boundaries) {
    auto probe = std::make_shared<FlushProbe>();
    std::stop_source cancelled;
    probe->cancel_during_flush = &cancelled;
    FakeVolumePlatform platform(probe);
    auto host = started_checkpoint_host();

    const auto outcome = checkpoint_volume_for_removal(
        platform,
        *host.core,
        synthetic_command("30000000-0000-4000-8000-000000000007"),
        synthetic_verified(),
        {cancelled.get_token(),
         std::chrono::steady_clock::time_point::max()});

    REQUIRE(cancelled.stop_requested());
    REQUIRE(outcome.volume_flush.ok());
    REQUIRE(outcome.error.has_value());
    REQUIRE(!outcome.checkpoint.has_value());
    REQUIRE(!outcome.proof.has_value());
    REQUIRE_EQ(host.factory->provider->checkpoint_calls, 0U);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{"open", "observe", "flush"}));
}

TEST_CASE(volume_checkpoint_for_removal_rejects_cross_binding_before_flush) {
    auto probe = std::make_shared<FlushProbe>();
    FakeVolumePlatform platform(probe);
    auto host = started_checkpoint_host();
    auto command = synthetic_command(
        "30000000-0000-4000-8000-000000000005");
    command.binding.session_generation++;

    const auto outcome = checkpoint_volume_for_removal(
        platform, *host.core, command, synthetic_verified(), {});

    REQUIRE(!outcome.ok());
    REQUIRE(outcome.error.has_value());
    REQUIRE(probe->events.empty());
    REQUIRE_EQ(host.factory->provider->checkpoint_calls, 0U);
}

TEST_CASE(volume_checkpoint_flush_rejects_opened_identity_change) {
    auto probe = std::make_shared<FlushProbe>();
    ++probe->observation.volume_serial;
    FakeVolumePlatform platform(probe);
    const auto outcome = flush_volume_for_checkpoint(
        platform, synthetic_verified(), {});
    REQUIRE(!outcome.ok());
    REQUIRE(outcome.error.has_value());
    REQUIRE(!outcome.flushed);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{"open", "observe"}));
}

TEST_CASE(volume_removal_orders_lock_flush_and_dismount) {
    auto probe = std::make_shared<FlushProbe>();
    FakeVolumePlatform platform(probe);
    const auto outcome = prepare_volume_for_removal(
        platform, successful_checkpoint(), {});
    REQUIRE(outcome.ok());
    REQUIRE(outcome.proof.has_value());
    REQUIRE_EQ(
        outcome.proof->purpose(),
        VolumeFlushPurpose::removal_after_checkpoint);
    REQUIRE(outcome.locked);
    REQUIRE(outcome.flushed);
    REQUIRE(outcome.dismounted);
    REQUIRE(!outcome.already_unmounted);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{
            "open", "observe", "is_mounted", "lock", "flush",
            "dismount"}));
}

TEST_CASE(volume_removal_busy_lock_never_flushes_or_dismounts) {
    auto probe = std::make_shared<FlushProbe>();
    probe->lock_error = fake_error(
        VolumeFlushError::volume_busy, L"FakeVolumeHandle::lock");
    FakeVolumePlatform platform(probe);
    const auto outcome = prepare_volume_for_removal(
        platform, successful_checkpoint(), {});
    REQUIRE(!outcome.ok());
    REQUIRE(outcome.error.has_value());
    REQUIRE(!outcome.locked);
    REQUIRE(!outcome.flushed);
    REQUIRE(!outcome.dismounted);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{
            "open", "observe", "is_mounted", "lock"}));
}

TEST_CASE(volume_removal_already_unmounted_still_flushes_exact_handle) {
    auto probe = std::make_shared<FlushProbe>();
    probe->mounted = false;
    FakeVolumePlatform platform(probe);
    const auto outcome = prepare_volume_for_removal(
        platform, successful_checkpoint(), {});
    REQUIRE(outcome.ok());
    REQUIRE(outcome.already_unmounted);
    REQUIRE(!outcome.locked);
    REQUIRE(outcome.flushed);
    REQUIRE(!outcome.dismounted);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{
            "open", "observe", "is_mounted", "flush"}));
}

TEST_CASE(volume_removal_flush_or_dismount_failure_attempts_unlock) {
    auto probe = std::make_shared<FlushProbe>();
    probe->flush_error = fake_error(
        VolumeFlushError::flush_failed, L"FakeVolumeHandle::flush");
    FakeVolumePlatform platform(probe);
    auto outcome = prepare_volume_for_removal(
        platform, successful_checkpoint(), {});
    REQUIRE(!outcome.ok());
    REQUIRE(outcome.locked);
    REQUIRE(!outcome.flushed);
    REQUIRE(outcome.unlock_attempted);
    REQUIRE(outcome.unlocked);
    REQUIRE_EQ(probe->events.back(), "unlock");

    probe = std::make_shared<FlushProbe>();
    probe->dismount_error = fake_error(
        VolumeFlushError::dismount_failed,
        L"FakeVolumeHandle::dismount");
    FakeVolumePlatform second_platform(probe);
    outcome = prepare_volume_for_removal(
        second_platform, successful_checkpoint(), {});
    REQUIRE(!outcome.ok());
    REQUIRE(outcome.flushed);
    REQUIRE(!outcome.dismounted);
    REQUIRE(outcome.unlock_attempted);
    REQUIRE(outcome.unlocked);
    REQUIRE_EQ(probe->events.back(), "unlock");
}

TEST_CASE(volume_removal_preserves_primary_and_unlock_cleanup_errors) {
    auto probe = std::make_shared<FlushProbe>();
    probe->flush_error = fake_error(
        VolumeFlushError::flush_failed, L"FakeVolumeHandle::flush");
    probe->unlock_error = fake_error(
        VolumeFlushError::unlock_failed, L"FakeVolumeHandle::unlock");
    FakeVolumePlatform platform(probe);
    const auto outcome = prepare_volume_for_removal(
        platform, successful_checkpoint(), {});
    REQUIRE(!outcome.ok());
    REQUIRE(outcome.error.has_value());
    REQUIRE(outcome.cleanup_error.has_value());
    REQUIRE_EQ(
        outcome.error->code,
        static_cast<std::uint32_t>(VolumeFlushError::flush_failed));
    REQUIRE_EQ(
        outcome.cleanup_error->code,
        static_cast<std::uint32_t>(VolumeFlushError::unlock_failed));
}

TEST_CASE(volume_flush_honors_cancel_and_deadline_before_mutation) {
    auto probe = std::make_shared<FlushProbe>();
    FakeVolumePlatform platform(probe);
    std::stop_source cancelled;
    cancelled.request_stop();
    auto outcome = flush_volume_for_checkpoint(
        platform,
        synthetic_verified(),
        {cancelled.get_token(),
         std::chrono::steady_clock::time_point::max()});
    REQUIRE(!outcome.ok());
    REQUIRE(probe->events.empty());

    outcome = flush_volume_for_checkpoint(
        platform,
        synthetic_verified(),
        {{}, std::chrono::steady_clock::now()});
    REQUIRE(!outcome.ok());
    REQUIRE(probe->events.empty());
}

TEST_CASE(volume_removal_cancellation_after_lock_unlocks_without_dismount) {
    auto probe = std::make_shared<FlushProbe>();
    std::stop_source cancelled;
    probe->cancel_during_lock = &cancelled;
    FakeVolumePlatform platform(probe);
    const auto outcome = prepare_volume_for_removal(
        platform,
        successful_checkpoint(),
        {cancelled.get_token(),
         std::chrono::steady_clock::time_point::max()});
    REQUIRE(!outcome.ok());
    REQUIRE(outcome.locked);
    REQUIRE(!outcome.flushed);
    REQUIRE(outcome.unlocked);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{
            "open", "observe", "is_mounted", "lock", "unlock"}));
}

TEST_CASE(volume_checkpoint_flush_completion_wins_late_cancellation) {
    auto probe = std::make_shared<FlushProbe>();
    std::stop_source cancelled;
    probe->cancel_during_flush = &cancelled;
    FakeVolumePlatform platform(probe);
    const auto outcome = flush_volume_for_checkpoint(
        platform,
        synthetic_verified(),
        {cancelled.get_token(),
         std::chrono::steady_clock::time_point::max()});
    REQUIRE(outcome.ok());
    REQUIRE(outcome.flushed);
    REQUIRE(cancelled.stop_requested());
}

TEST_CASE(volume_identity_windows_enumeration_requires_explicit_approved_tuple) {
    const auto enabled = environment(L"MYRD_M4_APPROVED_VOLUME_TEST");
    if (!enabled.has_value() || *enabled != L"1") return;

    const auto device_value = parse_u64(L"MYRD_M4_AIM_DEVICE_ID");
    const auto physical_number = parse_u64(L"MYRD_M4_PHYSICAL_DRIVE_NUMBER");
    const auto port_number = parse_u64(L"MYRD_M4_SCSI_PORT_NUMBER");
    REQUIRE(device_value <= (std::numeric_limits<std::uint32_t>::max)());
    REQUIRE(physical_number <= (std::numeric_limits<std::uint32_t>::max)());
    REQUIRE(port_number <= (std::numeric_limits<std::uint32_t>::max)());
    const aim::DeviceId device{static_cast<std::uint32_t>(device_value)};
    const auto address = aim::decode_device_address(device);
    const auto disk_length = parse_u64(L"MYRD_M4_DISK_LENGTH_BYTES");
    const auto number = static_cast<std::uint32_t>(physical_number);
    const VolumeIdentityExpectation expected{
        {fixed_uuid("00112233-4455-4677-8899-aabbccddeeff"),
         fixed_uuid("10213243-5465-4687-98a9-bacbdcedfe0f"),
         1,
         1,
         device},
        {number,
         L"\\\\.\\PhysicalDrive" + std::to_wstring(number),
         disk_length,
         static_cast<std::uint32_t>(port_number),
         address.path_id,
         address.target_id,
         address.lun},
        parse_uuid(L"MYRD_M4_GPT_DISK_GUID"),
        parse_uuid(L"MYRD_M4_PARTITION_GUID"),
        parse_uuid(L"MYRD_M4_VOLUME_GUID"),
        std::nullopt};

    const auto verified = verify_windows_volume_identity(expected);
    REQUIRE(verified.ok());
    REQUIRE_EQ(verified.value().owner(), expected.owner);
    REQUIRE_EQ(
        verified.value().observation().physical_disk,
        expected.physical_disk);
    REQUIRE(verified.value().pinned_expectation().volume_serial.has_value());

    const auto flush_enabled = environment(
        L"MYRD_M4_APPROVED_VOLUME_FLUSH_TEST");
    if (flush_enabled.has_value() && *flush_enabled == L"1") {
        auto platform = make_windows_volume_flush_platform();
        REQUIRE(platform != nullptr);
        const auto flushed = flush_volume_for_checkpoint(
            *platform, verified.value(), {});
        REQUIRE(flushed.ok());
        REQUIRE(flushed.flushed);
        REQUIRE(flushed.proof.has_value());
    }
}
