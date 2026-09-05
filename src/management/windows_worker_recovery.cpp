#include "src/management/windows_worker_recovery.h"

#include "src/common/sha256.h"
#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/physical_drive_locator.h"
#include "src/driver/windows_srb_transport.h"
#include "src/host/device_lifecycle.h"
#include "src/persistence/image_inspector.h"
#include "src/persistence/production_policy.h"
#include "src/profile/profile_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ntddscsi.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace myrd::management::windows {
namespace {

inline constexpr std::wstring_view kRestartHistoryFile =
    L"worker.restart";
inline constexpr std::wstring_view kRestartHistoryPreviousFile =
    L"worker.restart.prev";
inline constexpr std::array<std::byte, 8> kHistoryMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'W'}, std::byte{'R'}, std::byte{'H'}, std::byte{0}};
inline constexpr std::uint16_t kHistoryMajor = 1;
inline constexpr std::uint16_t kHistoryMinor = 0;
inline constexpr std::size_t kUuidTextBytes = 36;

enum class RecoveryAdapterError : std::uint32_t {
    invalid_record = 0x5C01,
    history_conflict,
    checkpoint_unresolved,
    device_observation_failed,
};

template <typename T>
Result<T> recovery_error(
    const RecoveryAdapterError code,
    const wchar_t* operation,
    const wchar_t* message,
    const std::uint32_t native = 0) {
    return Result<T>::failure({
        ErrorDomain::protocol, static_cast<std::uint32_t>(code), operation,
        message, native});
}

bool zero_uuid(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](const std::byte byte) { return byte == std::byte{}; });
}

template <typename T>
void append_le(std::vector<std::byte>& output, const T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(
            (value >> (index * 8U)) & static_cast<T>(0xffU)));
    }
}

template <typename T>
Result<T> read_le(std::span<const std::byte> bytes, std::size_t& offset) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        return recovery_error<T>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.decode",
            L"The durable worker restart record is truncated");
    }
    T value{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned char>(
            bytes[offset++])) << (index * 8U);
    }
    return Result<T>::success(value);
}

void append_uuid(
    std::vector<std::byte>& output,
    const product::Uuid& value) {
    const auto text = value.to_string();
    for (const auto character : text) {
        output.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(character)));
    }
}

Result<product::Uuid> read_uuid(
    std::span<const std::byte> bytes,
    std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < kUuidTextBytes) {
        return recovery_error<product::Uuid>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.decode_uuid",
            L"The durable worker restart UUID is truncated");
    }
    std::array<char, kUuidTextBytes> text{};
    for (auto& character : text) {
        character = static_cast<char>(std::to_integer<unsigned char>(
            bytes[offset++]));
    }
    auto parsed = product::Uuid::parse(
        std::string_view(text.data(), text.size()));
    if (!parsed.ok()) {
        return recovery_error<product::Uuid>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.decode_uuid",
            L"The durable worker restart UUID is malformed");
    }
    return parsed;
}

Result<std::vector<std::byte>> encode_history(
    const WorkerRestartHistory& history) {
    if (zero_uuid(history.profile_id) || history.revision == 0
        || history.attempts.empty()
        || history.attempts.size() > kMaximumAutomaticWorkerRestarts) {
        return recovery_error<std::vector<std::byte>>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.encode_history",
            L"The worker restart history is incomplete or unbounded");
    }
    try {
        std::vector<std::byte> output;
        output.reserve(512);
        output.insert(output.end(), kHistoryMagic.begin(), kHistoryMagic.end());
        append_le(output, kHistoryMajor);
        append_le(output, kHistoryMinor);
        const auto total_offset = output.size();
        append_le(output, std::uint32_t{});
        append_uuid(output, history.profile_id);
        append_le(output, history.revision);
        append_le(output, static_cast<std::uint32_t>(history.attempts.size()));
        for (const auto& attempt : history.attempts) {
            if (zero_uuid(attempt.image_id)
                || zero_uuid(attempt.recovery_operation_id)
                || attempt.attempted_at_unix_ms == 0
                || attempt.checkpoint_generation == 0) {
                return recovery_error<std::vector<std::byte>>(
                    RecoveryAdapterError::invalid_record,
                    L"windows_worker_recovery.encode_attempt",
                    L"A worker restart attempt is incomplete");
            }
            append_uuid(output, attempt.image_id);
            append_uuid(output, attempt.recovery_operation_id);
            append_le(output, attempt.attempted_at_unix_ms);
            append_le(output, attempt.checkpoint_generation);
            append_le(output, attempt.observed_dirty_epoch);
            append_le(output, static_cast<std::uint32_t>(
                attempt.volatile_data_loss_possible ? 1U : 0U));
        }
        if (output.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return recovery_error<std::vector<std::byte>>(
                RecoveryAdapterError::invalid_record,
                L"windows_worker_recovery.encode_history",
                L"The worker restart history exceeded its bounded format");
        }
        const auto total = static_cast<std::uint32_t>(output.size());
        for (std::size_t index = 0; index < sizeof(total); ++index) {
            output[total_offset + index] = static_cast<std::byte>(
                (total >> (index * 8U)) & 0xffU);
        }
        return Result<std::vector<std::byte>>::success(std::move(output));
    } catch (...) {
        return recovery_error<std::vector<std::byte>>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.encode_history",
            L"The worker restart history could not be allocated");
    }
}

Result<WorkerRestartHistory> decode_history(
    const std::span<const std::byte> bytes,
    const product::Uuid& expected_profile) {
    if (bytes.size() < kHistoryMagic.size()
        || !std::equal(kHistoryMagic.begin(), kHistoryMagic.end(), bytes.begin())) {
        return recovery_error<WorkerRestartHistory>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.decode_history",
            L"The worker restart history magic is invalid");
    }
    std::size_t offset = kHistoryMagic.size();
    auto major = read_le<std::uint16_t>(bytes, offset);
    auto minor = read_le<std::uint16_t>(bytes, offset);
    auto total = read_le<std::uint32_t>(bytes, offset);
    auto profile_id = read_uuid(bytes, offset);
    auto revision = read_le<std::uint64_t>(bytes, offset);
    auto count = read_le<std::uint32_t>(bytes, offset);
    if (!major.ok() || !minor.ok() || !total.ok() || !profile_id.ok()
        || !revision.ok() || !count.ok()
        || major.value() != kHistoryMajor || minor.value() != kHistoryMinor
        || total.value() != bytes.size()
        || profile_id.value() != expected_profile || revision.value() == 0
        || count.value() == 0
        || count.value() > kMaximumAutomaticWorkerRestarts) {
        return recovery_error<WorkerRestartHistory>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.decode_history",
            L"The worker restart history header is contradictory");
    }
    WorkerRestartHistory history{
        profile_id.move_value(), revision.value(), {}};
    try {
        history.attempts.reserve(count.value());
    } catch (...) {
        return recovery_error<WorkerRestartHistory>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.decode_history",
            L"The worker restart history could not be allocated");
    }
    for (std::uint32_t index = 0; index < count.value(); ++index) {
        auto image_id = read_uuid(bytes, offset);
        auto operation_id = read_uuid(bytes, offset);
        auto attempted_at = read_le<std::uint64_t>(bytes, offset);
        auto generation = read_le<std::uint64_t>(bytes, offset);
        auto dirty_epoch = read_le<std::uint64_t>(bytes, offset);
        auto flags = read_le<std::uint32_t>(bytes, offset);
        if (!image_id.ok() || !operation_id.ok() || !attempted_at.ok()
            || !generation.ok() || !dirty_epoch.ok() || !flags.ok()
            || zero_uuid(image_id.value()) || zero_uuid(operation_id.value())
            || attempted_at.value() == 0 || generation.value() == 0
            || (flags.value() & ~1U) != 0) {
            return recovery_error<WorkerRestartHistory>(
                RecoveryAdapterError::invalid_record,
                L"windows_worker_recovery.decode_attempt",
                L"A durable worker restart attempt is malformed");
        }
        history.attempts.push_back({
            image_id.move_value(), operation_id.move_value(),
            attempted_at.value(), generation.value(), dirty_epoch.value(),
            (flags.value() & 1U) != 0});
    }
    if (offset != bytes.size()) {
        return recovery_error<WorkerRestartHistory>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.decode_history",
            L"The worker restart history has trailing bytes");
    }
    return Result<WorkerRestartHistory>::success(std::move(history));
}

struct ObservedPhysicalDisk final {
    host::PhysicalDiskIdentity identity;
};

Result<std::vector<ObservedPhysicalDisk>> observe_physical_disks() {
    auto located = aim::enumerate_present_physical_drives();
    if (!located.ok()) {
        return Result<std::vector<ObservedPhysicalDisk>>::failure(
            located.error());
    }
    std::vector<ObservedPhysicalDisk> result;
    try {
        result.reserve(located.value().size());
    } catch (...) {
        return recovery_error<std::vector<ObservedPhysicalDisk>>(
            RecoveryAdapterError::device_observation_failed,
            L"windows_worker_recovery.observe_disks",
            L"Physical-drive observations could not be allocated");
    }
    for (const auto& disk : located.value()) {
        HANDLE handle = CreateFileW(
            disk.path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return recovery_error<std::vector<ObservedPhysicalDisk>>(
                RecoveryAdapterError::device_observation_failed,
                L"windows_worker_recovery.open_disk",
                L"A present PhysicalDrive could not be opened for identity",
                GetLastError());
        }
        SCSI_ADDRESS address{};
        DWORD returned{};
        const BOOL queried = DeviceIoControl(
            handle, IOCTL_SCSI_GET_ADDRESS, nullptr, 0,
            &address, sizeof(address), &returned, nullptr);
        const DWORD error = queried ? ERROR_SUCCESS : GetLastError();
        static_cast<void>(CloseHandle(handle));
        if (!queried) {
            return recovery_error<std::vector<ObservedPhysicalDisk>>(
                RecoveryAdapterError::device_observation_failed,
                L"windows_worker_recovery.query_disk",
                L"A present PhysicalDrive has no stable SCSI identity", error);
        }
        result.push_back({{
            disk.physical_drive_number, disk.path, disk.length_bytes,
            address.PortNumber, address.PathId, address.TargetId,
            address.Lun}});
    }
    return Result<std::vector<ObservedPhysicalDisk>>::success(
        std::move(result));
}

class WindowsAimResidualControl final : public host::IResidualDeviceControl {
public:
    static Result<std::unique_ptr<WindowsAimResidualControl>> create(
        const host::WorkerRuntimeStatus& persisted) {
        auto adapter = aim::discover_aim_adapter();
        if (!adapter.ok()) {
            return Result<std::unique_ptr<WindowsAimResidualControl>>::failure(
                adapter.error());
        }
        try {
            return Result<std::unique_ptr<WindowsAimResidualControl>>::success(
                std::unique_ptr<WindowsAimResidualControl>(
                    new WindowsAimResidualControl(
                        std::move(adapter.value()), persisted)));
        } catch (...) {
            return recovery_error<std::unique_ptr<WindowsAimResidualControl>>(
                RecoveryAdapterError::device_observation_failed,
                L"windows_worker_recovery.create_residual_control",
                L"The residual-device adapter could not be allocated");
        }
    }

    Result<std::vector<host::ResidualDeviceObservation>>
    enumerate_devices() override {
        auto ids = adapter_.list_devices();
        if (!ids.ok()) {
            return Result<std::vector<host::ResidualDeviceObservation>>::failure(
                ids.error());
        }
        auto disks = observe_physical_disks();
        if (!disks.ok()) {
            return Result<std::vector<host::ResidualDeviceObservation>>::failure(
                disks.error());
        }
        std::vector<host::ResidualDeviceObservation> observations;
        std::vector<std::uint32_t> mapped_drives;
        try {
            observations.reserve(ids.value().size() + 1);
            mapped_drives.reserve(ids.value().size());
        } catch (...) {
            return recovery_error<
                std::vector<host::ResidualDeviceObservation>>(
                RecoveryAdapterError::device_observation_failed,
                L"windows_worker_recovery.enumerate_residual",
                L"Residual-device observations could not be allocated");
        }
        for (const auto id : ids.value()) {
            auto info = adapter_.query_device(id);
            if (!info.ok()) {
                return Result<std::vector<host::ResidualDeviceObservation>>::failure(
                    info.error());
            }
            const auto address = aim::decode_device_address(id);
            const auto disk = std::find_if(
                disks.value().begin(), disks.value().end(),
                [&](const ObservedPhysicalDisk& candidate) {
                    return candidate.identity.port_number
                            == expected_.device.port_number
                        && candidate.identity.path_id == address.path_id
                        && candidate.identity.target_id == address.target_id
                        && candidate.identity.lun == address.lun;
                });
            host::PhysicalDiskIdentity physical{};
            if (disk != disks.value().end()) {
                physical = disk->identity;
                mapped_drives.push_back(physical.physical_drive_number);
            } else {
                physical.path_id = address.path_id;
                physical.target_id = address.target_id;
                physical.lun = address.lun;
                physical.port_number = expected_.device.port_number;
                physical.length_bytes = info.value().size_bytes;
            }
            const bool exact_registration =
                id.value == expected_.aim_device_id
                && info.value().native_object_path
                    == expected_.proxy_native_path;
            std::optional<host::HostOwnershipMarker> owner;
            if (exact_registration) owner = expected_owner();
            observations.push_back({
                id,
                aim::has_m1_product_proxy_flags(info.value().flags)
                    && info.value().native_object_path
                        == expected_.proxy_native_path,
                std::move(owner),
                std::move(physical)});
        }
        for (const auto& disk : disks.value()) {
            const bool mapped = std::find(
                mapped_drives.begin(), mapped_drives.end(),
                disk.identity.physical_drive_number) != mapped_drives.end();
            if (!mapped
                && (disk.identity.physical_drive_number
                        == expected_.device.physical_drive_number
                    || disk.identity.path == expected_physical().path)) {
                observations.push_back({
                    aim::DeviceId{aim::kAllDevices}, false, std::nullopt,
                    disk.identity});
            }
        }
        return Result<std::vector<host::ResidualDeviceObservation>>::success(
            std::move(observations));
    }

    Result<host::ResidualDeviceRemovalProof> remove_if_still_exact(
        const host::ResidualDeviceIdentity& expected) override {
        if (expected.owner != expected_owner()
            || expected.physical_disk != expected_physical()) {
            return recovery_error<host::ResidualDeviceRemovalProof>(
                RecoveryAdapterError::device_observation_failed,
                L"windows_worker_recovery.remove_binding",
                L"Residual-device removal request changed identity");
        }
        auto before = enumerate_devices();
        if (!before.ok()) {
            return Result<host::ResidualDeviceRemovalProof>::failure(
                before.error());
        }
        const auto exact = std::count_if(
            before.value().begin(), before.value().end(),
            [&](const host::ResidualDeviceObservation& value) {
                return value.device_id == expected.owner.aim_device_id
                    && value.product_proxy
                    && value.registered_owner == expected.owner
                    && value.physical_disk == expected.physical_disk;
            });
        if (exact != 1) {
            return recovery_error<host::ResidualDeviceRemovalProof>(
                RecoveryAdapterError::device_observation_failed,
                L"windows_worker_recovery.remove_revalidate",
                L"The residual device changed before exact removal");
        }
        auto removed = adapter_.remove_device(expected.owner.aim_device_id);
        if (!removed.ok()) {
            return Result<host::ResidualDeviceRemovalProof>::failure(
                removed.error());
        }
        auto rescanned = aim::rescan_aim_adapter(adapter_handle_.dev_inst);
        if (!rescanned.ok()) {
            return Result<host::ResidualDeviceRemovalProof>::failure(
                rescanned.error());
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(30);
        for (;;) {
            auto ids = adapter_.list_devices();
            auto disks = observe_physical_disks();
            if (!ids.ok()) {
                return Result<host::ResidualDeviceRemovalProof>::failure(
                    ids.error());
            }
            if (!disks.ok()) {
                return Result<host::ResidualDeviceRemovalProof>::failure(
                    disks.error());
            }
            const bool device_present = std::find(
                ids.value().begin(), ids.value().end(),
                expected.owner.aim_device_id) != ids.value().end();
            const bool disk_present = std::any_of(
                disks.value().begin(), disks.value().end(),
                [&](const ObservedPhysicalDisk& value) {
                    return value.identity.physical_drive_number
                            == expected.physical_disk.physical_drive_number
                        || value.identity.path == expected.physical_disk.path;
                });
            if (!device_present && !disk_present) {
                return Result<host::ResidualDeviceRemovalProof>::success(
                    {expected, true});
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return recovery_error<host::ResidualDeviceRemovalProof>(
                    RecoveryAdapterError::device_observation_failed,
                    L"windows_worker_recovery.remove_wait",
                    L"The exact AIM device or PhysicalDrive remained visible",
                    WAIT_TIMEOUT);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

private:
    WindowsAimResidualControl(
        aim::AdapterHandle adapter_handle,
        host::WorkerRuntimeStatus persisted)
        : adapter_handle_(std::move(adapter_handle)),
          transport_(adapter_handle_.handle.get()), adapter_(transport_),
          expected_(std::move(persisted)) {}

    host::HostOwnershipMarker expected_owner() const {
        return {
            expected_.binding.profile_id,
            expected_.binding.image_id,
            expected_.binding.profile_revision,
            expected_.binding.session_generation,
            aim::DeviceId{expected_.aim_device_id}};
    }

    host::PhysicalDiskIdentity expected_physical() const {
        return {
            expected_.device.physical_drive_number,
            std::wstring(L"\\\\.\\PhysicalDrive")
                + std::to_wstring(
                    expected_.device.physical_drive_number),
            expected_.device.length_bytes,
            expected_.device.port_number,
            expected_.device.path_id,
            expected_.device.target_id,
            expected_.device.lun};
    }

    aim::AdapterHandle adapter_handle_;
    aim::WindowsSrbTransport transport_;
    aim::DirectAimAdapter adapter_;
    host::WorkerRuntimeStatus expected_;
};

bool image_uuid_matches(
    const persistence::ImageUuid& actual,
    const product::Uuid& expected) noexcept {
    return std::equal(
        expected.bytes().begin(), expected.bytes().end(),
        actual.bytes.begin(), actual.bytes.end());
}

} // namespace

std::uint64_t SystemServiceClock::unix_time_ms() const noexcept {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    constexpr std::uint64_t windows_to_unix = 116444736000000000ULL;
    return value.QuadPart >= windows_to_unix
        ? (value.QuadPart - windows_to_unix) / 10'000ULL : 0;
}

std::uint64_t SystemServiceClock::monotonic_time_ms() const noexcept {
    return GetTickCount64();
}

DurableWorkerRestartHistoryStore::DurableWorkerRestartHistoryStore(
    std::wstring program_data)
    : program_data_(std::move(program_data)) {}

Result<std::optional<WorkerRestartHistory>>
DurableWorkerRestartHistoryStore::load(const product::Uuid& profile_id) {
    auto directory = profile::ProfileDirectoryCapability::open_existing(
        program_data_, profile_id);
    if (!directory.ok()) {
        return Result<std::optional<WorkerRestartHistory>>::failure(
            directory.error());
    }
    auto bytes = directory.value().read_file(kRestartHistoryFile);
    if (!bytes.ok()) {
        if (bytes.error().code == static_cast<std::uint32_t>(
                profile::ProfileStoreError::path_not_found)) {
            return Result<std::optional<WorkerRestartHistory>>::success(
                std::nullopt);
        }
        return Result<std::optional<WorkerRestartHistory>>::failure(
            bytes.error());
    }
    auto decoded = decode_history(bytes.value(), profile_id);
    if (!decoded.ok()) {
        return Result<std::optional<WorkerRestartHistory>>::failure(
            decoded.error());
    }
    return Result<std::optional<WorkerRestartHistory>>::success(
        decoded.move_value());
}

Result<void> DurableWorkerRestartHistoryStore::write_next(
    const product::Uuid& profile_id,
    const std::optional<std::uint64_t> expected_revision,
    const WorkerRestartHistory& next) {
    auto current = load(profile_id);
    if (!current.ok()) return Result<void>::failure(current.error());
    const auto observed_revision = current.value().has_value()
        ? std::optional<std::uint64_t>(current.value()->revision)
        : std::nullopt;
    const bool revision_advances = expected_revision.has_value()
        ? next.revision == expected_revision.value() + 1
        : next.revision == 1;
    if (observed_revision != expected_revision || !revision_advances
        || next.profile_id != profile_id) {
        return recovery_error<void>(
            RecoveryAdapterError::history_conflict,
            L"windows_worker_recovery.write_history",
            L"The durable worker restart history compare-and-swap failed");
    }
    auto encoded = encode_history(next);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
    auto directory = profile::ProfileDirectoryCapability::open_existing(
        program_data_, profile_id);
    if (!directory.ok()) return Result<void>::failure(directory.error());
    return profile::publish_atomically_with_previous(
        directory.value(), kRestartHistoryFile,
        kRestartHistoryPreviousFile, encoded.value());
}

Result<void> DurableWorkerRestartHistoryStore::clear(
    const product::Uuid& profile_id) {
    auto directory = profile::ProfileDirectoryCapability::open_existing(
        program_data_, profile_id);
    if (!directory.ok()) return Result<void>::failure(directory.error());
    constexpr std::array<std::wstring_view, 2> leaves{
        kRestartHistoryFile,
        kRestartHistoryPreviousFile};
    for (const auto leaf : leaves) {
        auto removed = directory.value().remove_file(leaf);
        if (!removed.ok()
            && removed.error().code != static_cast<std::uint32_t>(
                profile::ProfileStoreError::path_not_found)) {
            return removed;
        }
    }
    return directory.value().flush_directory();
}

Result<host::WorkerRuntimeStatus> load_persisted_worker_status(
    const std::wstring& program_data,
    const host::WorkerBinding& expected_binding) {
    auto directory = profile::ProfileDirectoryCapability::open_existing(
        program_data, expected_binding.profile_id);
    if (!directory.ok()) {
        return Result<host::WorkerRuntimeStatus>::failure(directory.error());
    }
    auto bytes = directory.value().read_file(host::kRuntimeIdentityFileName);
    if (!bytes.ok()) {
        return Result<host::WorkerRuntimeStatus>::failure(bytes.error());
    }
    return host::decode_persisted_worker_runtime_status(
        bytes.value(), expected_binding);
}

Result<product::Uuid> derive_worker_recovery_operation_id(
    const WorkerRuntimeIdentity& failed_worker) {
    if (!is_valid(failed_worker)) {
        return recovery_error<product::Uuid>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.derive_operation",
            L"The failed worker identity is invalid");
    }
    try {
        std::vector<std::byte> input;
        constexpr std::string_view domain =
            "MyRamDisk/worker-recovery-operation/v1";
        input.reserve(domain.size() + 64);
        for (const auto character : domain) {
            input.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(character)));
        }
        input.insert(
            input.end(), failed_worker.profile_id.bytes().begin(),
            failed_worker.profile_id.bytes().end());
        input.insert(
            input.end(), failed_worker.image_id.bytes().begin(),
            failed_worker.image_id.bytes().end());
        append_le(input, failed_worker.worker_pid);
        append_le(input, failed_worker.process_creation_time);
        append_le(input, failed_worker.session_generation);
        append_le(input, failed_worker.profile_revision);
        auto digest = sha256_digest(input);
        if (!digest.ok()) {
            return Result<product::Uuid>::failure(digest.error());
        }
        auto bytes = digest.value();
        bytes[6] = static_cast<std::byte>(
            (std::to_integer<unsigned char>(bytes[6]) & 0x0fU) | 0x50U);
        bytes[8] = static_cast<std::byte>(
            (std::to_integer<unsigned char>(bytes[8]) & 0x3fU) | 0x80U);
        constexpr char hex[] = "0123456789abcdef";
        std::string text;
        text.reserve(36);
        for (std::size_t index = 0; index < 16; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                text.push_back('-');
            }
            const auto value = std::to_integer<unsigned char>(bytes[index]);
            text.push_back(hex[value >> 4U]);
            text.push_back(hex[value & 0x0fU]);
        }
        return product::Uuid::parse(text);
    } catch (...) {
        return recovery_error<product::Uuid>(
            RecoveryAdapterError::invalid_record,
            L"windows_worker_recovery.derive_operation",
            L"The deterministic recovery operation could not be allocated");
    }
}

Result<WorkerCheckpointRecoveryProof> reconcile_interrupted_checkpoint(
    const profile::ProfileRecord& record,
    const host::WorkerRuntimeStatus& persisted,
    const WorkerRestartRecoveryRequest& request) {
    const auto& configured = record.profile;
    if (configured.profile_uuid != request.expected.profile_id
        || configured.image_uuid != request.expected.image_id
        || record.revision != request.expected.profile_revision
        || persisted.binding.profile_id != request.expected.profile_id
        || persisted.binding.image_id != request.expected.image_id
        || persisted.binding.session_generation
            != request.expected.session_generation
        || persisted.committed_generation
            != request.confirmed_checkpoint_generation) {
        return recovery_error<WorkerCheckpointRecoveryProof>(
            RecoveryAdapterError::checkpoint_unresolved,
            L"windows_worker_recovery.checkpoint_binding",
            L"Checkpoint recovery inputs disagree with durable identity");
    }
    persistence::ImageInspectOptions options;
    options.manifest_relative_name = configured.manifest_name;
    options.recovery.limits = persistence::production_persistence_limits(
        configured.logical_capacity_bytes);
    const OperationContext context{
        {}, std::chrono::steady_clock::now() + std::chrono::minutes(2)};
    auto inspected = persistence::inspect_image(
        context, configured.bundle_path, options);
    if (!inspected.ok()) {
        return Result<WorkerCheckpointRecoveryProof>::failure(
            inspected.error());
    }
    if (!inspected.value().selected.has_value()) {
        return Result<WorkerCheckpointRecoveryProof>::success({
            request.expected.profile_id, request.expected.image_id,
            request.recovery_operation_id, request.expected.profile_revision,
            request.expected.session_generation, request.observed_dirty_epoch,
            request.confirmed_checkpoint_generation, false});
    }
    const auto& selected = inspected.value().selected.value();
    const bool exact = image_uuid_matches(
            selected.image_uuid, configured.image_uuid)
        && selected.geometry.virtual_bytes
            == configured.logical_capacity_bytes
        && selected.geometry.logical_sector_size
            == configured.logical_sector_size
        && selected.geometry.physical_sector_size
            == configured.physical_sector_size
        && selected.checkpoint_generation
            >= request.confirmed_checkpoint_generation;
    return Result<WorkerCheckpointRecoveryProof>::success({
        request.expected.profile_id, request.expected.image_id,
        request.recovery_operation_id, request.expected.profile_revision,
        request.expected.session_generation, request.observed_dirty_epoch,
        exact ? selected.checkpoint_generation
              : request.confirmed_checkpoint_generation,
        exact});
}

Result<WorkerResidualDeviceRecoveryProof> reconcile_windows_residual_device(
    const host::WorkerRuntimeStatus& persisted,
    const WorkerRestartRecoveryRequest& request) {
    auto control = WindowsAimResidualControl::create(persisted);
    if (!control.ok()) {
        return Result<WorkerResidualDeviceRecoveryProof>::failure(
            control.error());
    }
    const host::ResidualDeviceIdentity expected{
        {request.expected.profile_id, request.expected.image_id,
         request.expected.profile_revision,
         request.expected.session_generation,
         aim::DeviceId{persisted.aim_device_id}},
        {request.expected_device.physical_drive_number,
         std::wstring(L"\\\\.\\PhysicalDrive")
             + std::to_wstring(
                 request.expected_device.physical_drive_number),
         request.expected_device.length_bytes,
         request.expected_device.port_number,
         request.expected_device.path_id,
         request.expected_device.target_id,
         request.expected_device.lun}};
    auto reconciled = host::reconcile_residual_device(
        expected, *control.value());
    if (!reconciled.ok()) {
        return Result<WorkerResidualDeviceRecoveryProof>::failure(
            reconciled.error());
    }
    WorkerResidualDeviceRecoveryDisposition disposition =
        WorkerResidualDeviceRecoveryDisposition::needs_attention;
    if (reconciled.value().disposition
        == host::ResidualDeviceDisposition::absent) {
        disposition = WorkerResidualDeviceRecoveryDisposition::absent;
    } else if (reconciled.value().disposition
        == host::ResidualDeviceDisposition::exact_device_removed) {
        disposition =
            WorkerResidualDeviceRecoveryDisposition::exact_device_removed;
    }
    return Result<WorkerResidualDeviceRecoveryProof>::success({
        request.expected.profile_id, request.expected.image_id,
        request.recovery_operation_id, request.expected.profile_revision,
        request.expected.session_generation, request.expected_device,
        disposition});
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<std::vector<std::byte>> encode_worker_restart_history_for_test(
    const WorkerRestartHistory& history) {
    return encode_history(history);
}

Result<WorkerRestartHistory> decode_worker_restart_history_for_test(
    const std::span<const std::byte> bytes,
    const product::Uuid& expected_profile) {
    return decode_history(bytes, expected_profile);
}
#endif

} // namespace myrd::management::windows
