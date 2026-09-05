#include "src/management/profile_registration_store.h"

#include "src/management/profile_registration_codec.h"
#include "src/profile/profile_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace myrd::management {
namespace {

constexpr std::wstring_view kStateCurrent = L"service_state.myrs";
constexpr std::wstring_view kStatePrevious = L"service_state.prev";

template <class T>
Result<T> store_error(const wchar_t* operation, const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(ServiceCoreError::dependency_failure),
        operation,
        message});
}

template <class T>
Result<T> profile_not_found(const wchar_t* operation) {
    return Result<T>::failure(Error{
        // IServiceProfileStore exposes ServiceCoreError values.  Preserve the
        // domain used by the interface contract so callers can distinguish a
        // missing registration from a storage failure during reconciliation.
        ErrorDomain::internal,
        static_cast<std::uint32_t>(ServiceCoreError::profile_not_found),
        operation,
        L"The requested profile is not registered"});
}

bool is_profile_path_not_found(const Error& error) noexcept {
    return error.code == static_cast<std::uint32_t>(
        profile::ProfileStoreError::path_not_found);
}

bool zero_uuid(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](std::byte byte) { return byte == std::byte{0}; });
}

bool nonzero_file_identity(const BundleFileIdentity& identity) noexcept {
    return identity.volume_serial != 0
        && std::any_of(
            identity.file_id.begin(), identity.file_id.end(),
            [](std::byte byte) { return byte != std::byte{0}; });
}

std::wstring wide_uuid(const product::Uuid& value) {
    const auto text = value.to_string();
    return std::wstring(text.begin(), text.end());
}

std::optional<product::Uuid> exact_uuid_leaf(std::wstring_view name) {
    if (name.size() != 36) return std::nullopt;
    std::string narrow;
    narrow.reserve(name.size());
    for (const auto value : name) {
        if (value > 0x7f) return std::nullopt;
        narrow.push_back(static_cast<char>(value));
    }
    auto parsed = product::Uuid::parse(narrow);
    if (!parsed.ok() || wide_uuid(parsed.value()) != name) {
        return std::nullopt;
    }
    return parsed.move_value();
}

bool equal_ignore_case(std::wstring_view left,
                       std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(MAXDWORD)
        || right.size() > static_cast<std::size_t>(MAXDWORD)) {
        return false;
    }
    return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()), TRUE)
        == CSTR_EQUAL;
}

std::shared_ptr<std::mutex> shared_registration_gate(std::wstring key) {
    static std::mutex registry_mutex;
    static std::map<std::wstring, std::weak_ptr<std::mutex>, std::less<>>
        registry;
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    std::scoped_lock lock(registry_mutex);
    if (const auto found = registry.find(key); found != registry.end()) {
        if (auto existing = found->second.lock()) return existing;
        registry.erase(found);
    }
    auto created = std::make_shared<std::mutex>();
    registry.emplace(std::move(key), created);
    for (auto iterator = registry.begin(); iterator != registry.end();) {
        if (iterator->second.expired()) {
            iterator = registry.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return created;
}

std::wstring recovery_leaf(const product::Uuid& operation_id) {
    return L"recovery-" + wide_uuid(operation_id) + L".myrr";
}

std::wstring recovery_previous_leaf(const product::Uuid& operation_id) {
    return L"recovery-" + wide_uuid(operation_id) + L".prev";
}

std::wstring forget_leaf(const product::Uuid& operation_id) {
    return L"forget-" + wide_uuid(operation_id) + L".myft";
}

std::wstring forget_previous_leaf(const product::Uuid& operation_id) {
    return L"forget-" + wide_uuid(operation_id) + L".prev";
}

ProfileState initial_state(const profile::ProfileRecord& record) noexcept {
    return registered_profile_state(
        record.profile.lifecycle,
        product::DiskState{
            product::RuntimeState::stopped,
            product::ActivityState::idle,
            product::PersistenceState::clean,
            product::HealthState::healthy});
}

DurableProfileStateRecord initial_durable_state(
    const profile::ProfileRecord& record) noexcept {
    return DurableProfileStateRecord{
        0,
        initial_state(record),
        idle_operation_gate(),
        CheckpointDurableState{},
        CompactionDurableState{},
        StopDurableState{}};
}

Result<BundleFileIdentity> inspect_bundle_identity(
    std::wstring_view path) {
    if (path.empty() || path.find(L'\0') != std::wstring_view::npos) {
        return store_error<BundleFileIdentity>(
            L"profile_registration.bundle_identity",
            L"The registered bundle path is invalid");
    }
    const std::wstring terminated(path);
    const HANDLE raw = CreateFileW(
        terminated.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return Result<BundleFileIdentity>::failure(Error{
            ErrorDomain::win32,
            static_cast<std::uint32_t>(ServiceCoreError::dependency_failure),
            L"profile_registration.bundle_identity",
            L"The registered bundle cannot be opened for identity inspection",
            GetLastError()});
    }
    struct HandleGuard final {
        HANDLE value;
        ~HandleGuard() noexcept { static_cast<void>(CloseHandle(value)); }
    } guard{raw};

    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO identity{};
    if (!GetFileInformationByHandleEx(
            raw, FileAttributeTagInfo, &tag, sizeof(tag))
        || !GetFileInformationByHandleEx(
            raw, FileStandardInfo, &standard, sizeof(standard))
        || !GetFileInformationByHandleEx(
            raw, FileIdInfo, &identity, sizeof(identity))) {
        return Result<BundleFileIdentity>::failure(Error{
            ErrorDomain::win32,
            static_cast<std::uint32_t>(ServiceCoreError::dependency_failure),
            L"profile_registration.bundle_identity",
            L"The registered bundle identity cannot be queried",
            GetLastError()});
    }
    if (GetFileType(raw) != FILE_TYPE_DISK || standard.Directory
        || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || tag.ReparseTag != 0 || standard.NumberOfLinks != 1) {
        return store_error<BundleFileIdentity>(
            L"profile_registration.bundle_identity",
            L"A registered bundle must be one regular non-reparse disk file with one link");
    }
    BundleFileIdentity result{};
    result.volume_serial = identity.VolumeSerialNumber;
    std::transform(
        std::begin(identity.FileId.Identifier),
        std::end(identity.FileId.Identifier), result.file_id.begin(),
        [](unsigned char value) { return static_cast<std::byte>(value); });
    if (!nonzero_file_identity(result)) {
        return store_error<BundleFileIdentity>(
            L"profile_registration.bundle_identity",
            L"The registered bundle has an invalid file identity");
    }
    return Result<BundleFileIdentity>::success(result);
}

Result<profile::ProfileDirectoryCapability> open_profile_directory(
    std::wstring_view root,
    const product::Uuid& profile_id) {
    return profile::ProfileDirectoryCapability::open_existing(root, profile_id);
}

Result<profile::ProfileStore> open_profile_store(
    std::wstring_view root,
    const product::Uuid& profile_id) {
    return profile::ProfileStore::open_existing(root, profile_id);
}

template <class T>
struct DecodedDurableFile final {
    std::optional<T> value;
    std::optional<Error> corruption;
};

bool allows_previous_fallback(const Error& error) noexcept {
    if (error.domain != ErrorDomain::protocol) return false;
    switch (static_cast<ProfileRegistrationCodecError>(error.code)) {
    case ProfileRegistrationCodecError::invalid_size:
    case ProfileRegistrationCodecError::invalid_magic:
    case ProfileRegistrationCodecError::invalid_header:
    case ProfileRegistrationCodecError::checksum_mismatch:
    case ProfileRegistrationCodecError::invalid_fields:
    case ProfileRegistrationCodecError::text_encoding:
        return true;
    case ProfileRegistrationCodecError::unsupported_schema:
    default:
        return false;
    }
}

template <class T, class Decode>
Result<DecodedDurableFile<T>> read_decoded_durable_file(
    profile::IAtomicFileDirectory& directory,
    std::wstring_view leaf,
    Decode decode) {
    auto bytes = directory.read_file(leaf);
    if (!bytes.ok()) {
        if (is_profile_path_not_found(bytes.error())) {
            return Result<DecodedDurableFile<T>>::success({});
        }
        return Result<DecodedDurableFile<T>>::failure(bytes.error());
    }
    auto decoded = decode(bytes.value());
    if (!decoded.ok()) {
        if (!allows_previous_fallback(decoded.error())) {
            return Result<DecodedDurableFile<T>>::failure(decoded.error());
        }
        DecodedDurableFile<T> result;
        result.corruption = decoded.error();
        return Result<DecodedDurableFile<T>>::success(std::move(result));
    }
    DecodedDurableFile<T> result;
    result.value = decoded.move_value();
    return Result<DecodedDurableFile<T>>::success(std::move(result));
}

Result<DurableProfileStateRecord> load_durable_state(
    std::wstring_view root,
    const profile::ProfileRecord& record) {
    auto directory = open_profile_directory(
        root, record.profile.profile_uuid);
    if (!directory.ok()) {
        return Result<DurableProfileStateRecord>::failure(directory.error());
    }
    const auto decode = [](std::span<const std::byte> bytes) {
        return decode_profile_state_record(bytes);
    };
    auto current = read_decoded_durable_file<DurableProfileStateRecord>(
        directory.value(), kStateCurrent, decode);
    if (!current.ok()) {
        return Result<DurableProfileStateRecord>::failure(current.error());
    }
    auto previous = read_decoded_durable_file<DurableProfileStateRecord>(
        directory.value(), kStatePrevious, decode);
    std::optional<DurableProfileStateRecord> selected;
    if (current.value().value.has_value()) {
        selected = current.value().value.value();
        if (previous.ok() && previous.value().value.has_value()) {
            const auto& prior = previous.value().value.value();
            if (prior.revision > selected->revision) {
                selected = prior;
            } else if (prior.revision == selected->revision
                       && prior != selected.value()) {
                return store_error<DurableProfileStateRecord>(
                    L"profile_registration.load_state",
                    L"Current and previous service states conflict at one revision");
            }
        }
    } else if (!previous.ok()) {
        return Result<DurableProfileStateRecord>::failure(previous.error());
    } else if (previous.value().value.has_value()) {
        selected = previous.value().value.value();
    } else if (current.value().corruption.has_value()) {
        return Result<DurableProfileStateRecord>::failure(
            current.value().corruption.value());
    } else if (previous.value().corruption.has_value()) {
        return Result<DurableProfileStateRecord>::failure(
            previous.value().corruption.value());
    } else {
        return Result<DurableProfileStateRecord>::success(
            initial_durable_state(record));
    }
    if (!selected->state.lifecycle.has_value()
        || selected->state.lifecycle.value()
            != record.profile.lifecycle) {
        return store_error<DurableProfileStateRecord>(
            L"profile_registration.load_state",
            L"The durable service state does not match the profile lifecycle");
    }
    return Result<DurableProfileStateRecord>::success(std::move(*selected));
}

Result<ManagedProfileSnapshot> compose_snapshot(
    const profile::ProfileRecord& record,
    const DurableProfileStateRecord& state) {
    if (!state.state.lifecycle.has_value()
        || state.state.lifecycle.value() != record.profile.lifecycle) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.compose",
            L"The profile and durable service state disagree");
    }
    return Result<ManagedProfileSnapshot>::success(ManagedProfileSnapshot{
        record,
        state.state,
        state.operation_gate,
        state.checkpoint,
        state.compaction,
        state.stop});
}

Result<ManagedProfileSnapshot> load_snapshot(
    std::wstring_view root,
    const product::Uuid& profile_id) {
    auto store = open_profile_store(root, profile_id);
    if (!store.ok()) {
        if (is_profile_path_not_found(store.error())) {
            return profile_not_found<ManagedProfileSnapshot>(
                L"profile_registration.load");
        }
        return Result<ManagedProfileSnapshot>::failure(store.error());
    }
    auto record = store.value().load_current();
    if (!record.ok()) {
        if (is_profile_path_not_found(record.error())) {
            return profile_not_found<ManagedProfileSnapshot>(
                L"profile_registration.load");
        }
        return Result<ManagedProfileSnapshot>::failure(record.error());
    }
    auto state = load_durable_state(root, record.value());
    if (!state.ok()) {
        return Result<ManagedProfileSnapshot>::failure(state.error());
    }
    return compose_snapshot(record.value(), state.value());
}

Result<void> publish_state_bytes(
    std::wstring_view root,
    const product::Uuid& profile_id,
    const DurableProfileStateRecord& proposed) {
    if (!validate_durable_profile_state_record(proposed)) {
        return store_error<void>(
            L"profile_registration.publish_state",
            L"The proposed durable service state is semantically contradictory");
    }
    auto encoded = encode_profile_state_record(proposed);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
    auto directory = open_profile_directory(root, profile_id);
    if (!directory.ok()) return Result<void>::failure(directory.error());
    const auto decode = [](std::span<const std::byte> bytes) {
        return decode_profile_state_record(bytes);
    };
    auto current = read_decoded_durable_file<DurableProfileStateRecord>(
        directory.value(), kStateCurrent, decode);
    if (!current.ok()) return Result<void>::failure(current.error());
    bool discard_current = current.value().corruption.has_value();
    auto previous = read_decoded_durable_file<DurableProfileStateRecord>(
        directory.value(), kStatePrevious, decode);
    if (discard_current) {
        if (!previous.ok()) return Result<void>::failure(previous.error());
        if (!previous.value().value.has_value()
            || previous.value().value->revision
                == (std::numeric_limits<std::uint64_t>::max)()
            || previous.value().value->revision + 1 != proposed.revision) {
            return Result<void>::failure(
                current.value().corruption.value());
        }
    } else if (current.value().value.has_value() && previous.ok()
               && previous.value().value.has_value()
               && previous.value().value->revision
                    > current.value().value->revision) {
        if (previous.value().value->revision
                == (std::numeric_limits<std::uint64_t>::max)()
            || previous.value().value->revision + 1 != proposed.revision) {
            return store_error<void>(
                L"profile_registration.publish_state",
                L"The previous service state cannot advance to the proposed revision");
        }
        discard_current = true;
    }
    if (discard_current) {
        auto removed = directory.value().remove_file(kStateCurrent);
        if (!removed.ok()) return removed;
    }
    return profile::publish_atomically_with_previous(
        directory.value(), kStateCurrent, kStatePrevious, encoded.value());
}

Result<ManagedProfileSnapshot> publish_snapshot_state(
    std::wstring_view root,
    const ManagedProfileSnapshot& expected,
    const ProfileState& state,
    const OperationGateState& gate,
    const CheckpointDurableState& checkpoint,
    const CompactionDurableState& compaction,
    const StopDurableState& stop) {
    auto current = load_snapshot(
        root, expected.record.profile.profile_uuid);
    if (!current.ok()) return current;
    if (current.value() != expected) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.publish",
            L"The expected durable profile snapshot is stale");
    }
    if (!is_valid(state) || !state.lifecycle.has_value()
        || state.lifecycle.value() != expected.record.profile.lifecycle) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.publish",
            L"The proposed durable profile state is invalid");
    }
    auto durable = load_durable_state(root, expected.record);
    if (!durable.ok()) {
        return Result<ManagedProfileSnapshot>::failure(durable.error());
    }
    if (durable.value().revision
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.publish",
            L"The durable service-state revision is exhausted");
    }
    DurableProfileStateRecord proposed{
        durable.value().revision + 1,
        state,
        gate,
        checkpoint,
        compaction,
        stop};
    auto published = publish_state_bytes(
        root, expected.record.profile.profile_uuid, proposed);
    if (!published.ok()) {
        return Result<ManagedProfileSnapshot>::failure(published.error());
    }
    return compose_snapshot(expected.record, proposed);
}

Result<void> remove_if_present(
    profile::IAtomicFileDirectory& directory,
    std::wstring_view leaf) {
    auto removed = directory.remove_file(leaf);
    if (!removed.ok() && !is_profile_path_not_found(removed.error())) {
        return removed;
    }
    return Result<void>::success();
}

} // namespace

struct ProfileRegistrationStore::Impl final {
    std::wstring trusted_root;
    std::unique_ptr<profile::IAtomicFileDirectory> profiles_directory;
    profile::IProfileRepairBindingVerifier* repair_verifier{};
    std::shared_ptr<std::mutex> gate;
};

namespace {

template <class ImplT>
Result<std::optional<RecoveryCommandReservation>> find_recovery_locked(
    ImplT& impl,
    const product::Uuid& operation_id) {
    const auto decode = [](std::span<const std::byte> bytes) {
        return decode_recovery_command_reservation(bytes);
    };
    auto current = read_decoded_durable_file<RecoveryCommandReservation>(
        *impl.profiles_directory, recovery_leaf(operation_id), decode);
    if (!current.ok()) {
        return Result<std::optional<RecoveryCommandReservation>>::failure(
            current.error());
    }
    if (current.value().value.has_value()) {
        if (current.value().value->operation_id != operation_id) {
            return store_error<std::optional<RecoveryCommandReservation>>(
                L"profile_registration.recovery_load",
                L"The recovery reservation filename and identity disagree");
        }
        return Result<std::optional<RecoveryCommandReservation>>::success(
            current.value().value.value());
    }
    auto previous = read_decoded_durable_file<RecoveryCommandReservation>(
        *impl.profiles_directory, recovery_previous_leaf(operation_id), decode);
    if (!previous.ok()) {
        return Result<std::optional<RecoveryCommandReservation>>::failure(
            previous.error());
    }
    if (previous.value().value.has_value()) {
        if (previous.value().value->operation_id != operation_id) {
            return store_error<std::optional<RecoveryCommandReservation>>(
                L"profile_registration.recovery_load",
                L"The recovery reservation filename and identity disagree");
        }
        return Result<std::optional<RecoveryCommandReservation>>::success(
            previous.value().value.value());
    }
    if (current.value().corruption.has_value()) {
        return Result<std::optional<RecoveryCommandReservation>>::failure(
            current.value().corruption.value());
    }
    if (previous.value().corruption.has_value()) {
        return Result<std::optional<RecoveryCommandReservation>>::failure(
            previous.value().corruption.value());
    }
    return Result<std::optional<RecoveryCommandReservation>>::success(
        std::nullopt);
}

template <class ImplT>
Result<std::optional<ForgetTombstone>> load_tombstone_locked(
    ImplT& impl,
    const product::Uuid& operation_id) {
    const auto decode = [](std::span<const std::byte> bytes) {
        return decode_forget_tombstone(bytes);
    };
    auto current = read_decoded_durable_file<ForgetTombstone>(
        *impl.profiles_directory, forget_leaf(operation_id), decode);
    if (!current.ok()) {
        return Result<std::optional<ForgetTombstone>>::failure(
            current.error());
    }
    if (current.value().value.has_value()) {
        if (current.value().value->operation_id != operation_id) {
            return store_error<std::optional<ForgetTombstone>>(
                L"profile_registration.forget_load",
                L"The forget tombstone filename and identity disagree");
        }
        return Result<std::optional<ForgetTombstone>>::success(
            current.value().value.value());
    }
    auto previous = read_decoded_durable_file<ForgetTombstone>(
        *impl.profiles_directory, forget_previous_leaf(operation_id), decode);
    if (!previous.ok()) {
        return Result<std::optional<ForgetTombstone>>::failure(
            previous.error());
    }
    if (previous.value().value.has_value()) {
        if (previous.value().value->operation_id != operation_id) {
            return store_error<std::optional<ForgetTombstone>>(
                L"profile_registration.forget_load",
                L"The forget tombstone filename and identity disagree");
        }
        return Result<std::optional<ForgetTombstone>>::success(
            previous.value().value.value());
    }
    if (current.value().corruption.has_value()) {
        return Result<std::optional<ForgetTombstone>>::failure(
            current.value().corruption.value());
    }
    if (previous.value().corruption.has_value()) {
        return Result<std::optional<ForgetTombstone>>::failure(
            previous.value().corruption.value());
    }
    return Result<std::optional<ForgetTombstone>>::success(std::nullopt);
}

template <class ImplT>
Result<void> publish_tombstone_locked(
    ImplT& impl,
    const ForgetTombstone& tombstone) {
    auto encoded = encode_forget_tombstone(tombstone);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
    const auto decode = [](std::span<const std::byte> bytes) {
        return decode_forget_tombstone(bytes);
    };
    auto current = read_decoded_durable_file<ForgetTombstone>(
        *impl.profiles_directory, forget_leaf(tombstone.operation_id), decode);
    if (!current.ok()) return Result<void>::failure(current.error());
    if (current.value().corruption.has_value()) {
        auto previous = read_decoded_durable_file<ForgetTombstone>(
            *impl.profiles_directory,
            forget_previous_leaf(tombstone.operation_id), decode);
        if (!previous.ok()) return Result<void>::failure(previous.error());
        if (!previous.value().value.has_value()) {
            return Result<void>::failure(current.value().corruption.value());
        }
        auto expected_previous = tombstone;
        expected_previous.phase = previous.value().value->phase;
        if (previous.value().value.value() != expected_previous
            || previous.value().value->phase > tombstone.phase) {
            return store_error<void>(
                L"profile_registration.forget_publish",
                L"The previous forget tombstone cannot advance to the proposed phase");
        }
        auto removed = impl.profiles_directory->remove_file(
            forget_leaf(tombstone.operation_id));
        if (!removed.ok()) return removed;
    }
    return profile::publish_atomically_with_previous(
        *impl.profiles_directory,
        forget_leaf(tombstone.operation_id),
        forget_previous_leaf(tombstone.operation_id),
        encoded.value());
}

template <class ImplT>
Result<ForgetTombstone> complete_forget_removal_locked(
    ImplT& impl,
    ForgetTombstone tombstone) {
    if (tombstone.phase != ForgetTombstonePhase::removal_started
        || !tombstone.retained_bundle_identity.has_value()) {
        return store_error<ForgetTombstone>(
            L"profile_registration.forget_complete",
            L"Forget removal has no durable authorization boundary");
    }
    auto retained = inspect_bundle_identity(tombstone.retained_bundle_path);
    if (!retained.ok()) {
        return Result<ForgetTombstone>::failure(retained.error());
    }
    if (retained.value() != tombstone.retained_bundle_identity.value()) {
        return store_error<ForgetTombstone>(
            L"profile_registration.forget_complete",
            L"The retained bundle identity changed during metadata removal");
    }
    auto directory = open_profile_directory(
        impl.trusted_root, tombstone.profile_id);
    if (!directory.ok()) {
        return Result<ForgetTombstone>::failure(directory.error());
    }
    for (const auto leaf : {
             std::wstring_view(L"runtime.lease"),
             kStatePrevious,
             kStateCurrent,
             std::wstring_view(L"profile.prev"),
             std::wstring_view(L"profile.myrd")}) {
        auto removed = remove_if_present(directory.value(), leaf);
        if (!removed.ok()) {
            return Result<ForgetTombstone>::failure(removed.error());
        }
    }
    auto flushed = directory.value().flush_directory();
    if (!flushed.ok()) {
        return Result<ForgetTombstone>::failure(flushed.error());
    }
    tombstone.phase = ForgetTombstonePhase::metadata_removed;
    auto published = publish_tombstone_locked(impl, tombstone);
    if (!published.ok()) {
        return Result<ForgetTombstone>::failure(published.error());
    }
    return Result<ForgetTombstone>::success(std::move(tombstone));
}

template <class ImplT>
Result<ManagedProfileSnapshot> repair_candidate_locked(
    ImplT& impl,
    const product::Uuid& profile_id) {
    auto store = open_profile_store(impl.trusted_root, profile_id);
    if (!store.ok()) return Result<ManagedProfileSnapshot>::failure(store.error());

    auto current = store.value().load_current();
    if (current.ok()) {
        auto names = impl.profiles_directory->list_files();
        if (!names.ok()) {
            return Result<ManagedProfileSnapshot>::failure(names.error());
        }
        std::optional<RecoveryCommandReservation> matching;
        for (const auto& name : names.value()) {
            if (!name.starts_with(L"recovery-") || !name.ends_with(L".myrr")) {
                continue;
            }
            auto bytes = impl.profiles_directory->read_file(name);
            if (!bytes.ok()) return Result<ManagedProfileSnapshot>::failure(bytes.error());
            auto decoded = decode_recovery_command_reservation(bytes.value());
            if (!decoded.ok()) return Result<ManagedProfileSnapshot>::failure(decoded.error());
            const auto& candidate = decoded.value();
            if (candidate.intent.kind != RecoveryOperationKind::repair_profile
                || candidate.intent.profile_id != profile_id
                || candidate.expected_profile.profile != current.value().profile
                || candidate.expected_profile.revision
                    == (std::numeric_limits<std::uint64_t>::max)()
                || current.value().revision
                    != candidate.expected_profile.revision + 1) {
                continue;
            }
            if (matching.has_value()) {
                return store_error<ManagedProfileSnapshot>(
                    L"profile_registration.repair_candidate",
                    L"More than one repair reservation matches the current profile");
            }
            matching = candidate;
        }
        if (!matching.has_value()) {
            return store_error<ManagedProfileSnapshot>(
                L"profile_registration.repair_candidate",
                L"The current profile is valid and has no interrupted repair");
        }
        auto state = load_durable_state(
            impl.trusted_root, matching->expected_profile);
        if (!state.ok()) return Result<ManagedProfileSnapshot>::failure(state.error());
        auto snapshot = compose_snapshot(
            matching->expected_profile, state.value());
        if (!snapshot.ok()) return snapshot;
        snapshot.value().state = matching->before_state;
        snapshot.value().operation_gate = idle_operation_gate();
        return snapshot;
    }
    if (current.error().code != static_cast<std::uint32_t>(
            profile::ProfileStoreError::current_corrupt)) {
        return Result<ManagedProfileSnapshot>::failure(current.error());
    }
    auto claimed = store.value().claimed_current_revision();
    if (!claimed.ok()) {
        return Result<ManagedProfileSnapshot>::failure(claimed.error());
    }
    auto previous = store.value().load_previous();
    if (!previous.ok()) {
        return Result<ManagedProfileSnapshot>::failure(previous.error());
    }
    previous.value().revision = (std::max)(
        previous.value().revision, claimed.value());
    auto state = load_durable_state(impl.trusted_root, previous.value());
    if (!state.ok()) return Result<ManagedProfileSnapshot>::failure(state.error());
    // A corrupt current profile is the evidence that admits the explicit
    // repair path.  Keep the durable runtime/persistence truth, but project
    // the health state required by the state-machine contract.  In
    // particular, do not manufacture Stopped if a durable record says that
    // runtime ownership still needs reconciliation.
    state.value().state.disk.health = product::HealthState::needs_attention;
    return compose_snapshot(previous.value(), state.value());
}

} // namespace

ProfileRegistrationStore::ProfileRegistrationStore(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
ProfileRegistrationStore::ProfileRegistrationStore(
    ProfileRegistrationStore&&) noexcept = default;
ProfileRegistrationStore& ProfileRegistrationStore::operator=(
    ProfileRegistrationStore&&) noexcept = default;
ProfileRegistrationStore::~ProfileRegistrationStore() noexcept = default;

Result<ProfileRegistrationStore> ProfileRegistrationStore::open_existing(
    std::wstring_view trusted_program_data_root,
    profile::IProfileRepairBindingVerifier& repair_verifier) {
    try {
        constexpr std::array<std::wstring_view, 2> components{
            L"MyRamDisk", L"profiles"};
        auto managed = profile::open_managed_atomic_directory(
            trusted_program_data_root, components);
        if (!managed.ok()) {
            return Result<ProfileRegistrationStore>::failure(managed.error());
        }
        auto impl = std::make_unique<Impl>();
        impl->trusted_root = trusted_program_data_root;
        impl->gate = shared_registration_gate(managed.value().path);
        impl->profiles_directory = std::move(managed.value().directory);
        impl->repair_verifier = &repair_verifier;
        return Result<ProfileRegistrationStore>::success(
            ProfileRegistrationStore(std::move(impl)));
    } catch (...) {
        return store_error<ProfileRegistrationStore>(
            L"profile_registration.open",
            L"Failed to allocate production profile registration state");
    }
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::resolve(
    std::wstring_view target) {
    std::scoped_lock lock(*impl_->gate);
    auto names = impl_->profiles_directory->list_files();
    if (!names.ok()) return Result<ManagedProfileSnapshot>::failure(names.error());
    std::optional<ManagedProfileSnapshot> match;
    for (const auto& name : names.value()) {
        auto profile_id = exact_uuid_leaf(name);
        if (!profile_id.has_value()) continue;
        auto candidate = load_snapshot(impl_->trusted_root, profile_id.value());
        if (!candidate.ok()) {
            if (candidate.error().code == static_cast<std::uint32_t>(
                    ServiceCoreError::profile_not_found)) {
                continue;
            }
            return candidate;
        }
        if (!equal_ignore_case(candidate.value().record.profile.display_name, target)
            && !equal_ignore_case(wide_uuid(profile_id.value()), target)) {
            continue;
        }
        if (match.has_value()) {
            return store_error<ManagedProfileSnapshot>(
                L"profile_registration.resolve",
                L"The profile target is not unique");
        }
        match = candidate.move_value();
    }
    if (!match.has_value()) {
        return profile_not_found<ManagedProfileSnapshot>(
            L"profile_registration.resolve");
    }
    return Result<ManagedProfileSnapshot>::success(std::move(match.value()));
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::load(
    const product::Uuid& profile_id) {
    std::scoped_lock lock(*impl_->gate);
    return load_snapshot(impl_->trusted_root, profile_id);
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::publish_runtime(
    const ManagedProfileSnapshot& expected,
    const ProfileState& proposed_state,
    const OperationGateState& proposed_gate) {
    std::scoped_lock lock(*impl_->gate);
    return publish_snapshot_state(
        impl_->trusted_root, expected, proposed_state, proposed_gate,
        expected.checkpoint, expected.compaction, expected.stop);
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::publish_checkpoint(
    const ManagedProfileSnapshot& expected,
    const ProfileState& proposed_state,
    const OperationGateState& proposed_gate,
    const CheckpointDurableState& proposed_checkpoint) {
    std::scoped_lock lock(*impl_->gate);
    return publish_snapshot_state(
        impl_->trusted_root, expected, proposed_state, proposed_gate,
        proposed_checkpoint, expected.compaction, expected.stop);
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::publish_compaction(
    const ManagedProfileSnapshot& expected,
    const ProfileState& proposed_state,
    const OperationGateState& proposed_gate,
    const CheckpointDurableState& proposed_checkpoint,
    const CompactionDurableState& proposed_compaction) {
    std::scoped_lock lock(*impl_->gate);
    return publish_snapshot_state(
        impl_->trusted_root, expected, proposed_state, proposed_gate,
        proposed_checkpoint, proposed_compaction, expected.stop);
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::publish_stop(
    const ManagedProfileSnapshot& expected,
    const ProfileState& proposed_state,
    const OperationGateState& proposed_gate,
    const CheckpointDurableState& proposed_checkpoint,
    const CompactionDurableState& proposed_compaction,
    const StopDurableState& proposed_stop) {
    std::scoped_lock lock(*impl_->gate);
    return publish_snapshot_state(
        impl_->trusted_root, expected, proposed_state, proposed_gate,
        proposed_checkpoint, proposed_compaction, proposed_stop);
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::publish_rebind(
    const ManagedProfileSnapshot& expected,
    const profile::ProfileRecord& proposed,
    const BundleRegistrationEvidence& verified_bundle,
    const ProfileState& proposed_state,
    const OperationGateState& proposed_gate) {
    std::scoped_lock lock(*impl_->gate);
    auto current = load_snapshot(
        impl_->trusted_root, expected.record.profile.profile_uuid);
    if (!current.ok()) return current;
    if (current.value() != expected || proposed_state != expected.state
        || proposed_gate != expected.operation_gate
        || verified_bundle.source != BundleEvidenceSource::read_only_inspection
        || verified_bundle.image_id != proposed.profile.image_uuid
        || verified_bundle.manifest_name != proposed.profile.manifest_name
        || verified_bundle.logical_capacity_bytes
            != proposed.profile.logical_capacity_bytes
        || verified_bundle.logical_sector_bytes
            != proposed.profile.logical_sector_size
        || verified_bundle.physical_sector_bytes
            != proposed.profile.physical_sector_size
        || verified_bundle.payload_block_bytes != kProductPayloadBlockBytes
        || !nonzero_file_identity(verified_bundle.file_identity)) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.rebind",
            L"The rebind proposal or verified bundle evidence is stale");
    }
    auto observed = inspect_bundle_identity(proposed.profile.bundle_path);
    if (!observed.ok()) {
        return Result<ManagedProfileSnapshot>::failure(observed.error());
    }
    if (observed.value() != verified_bundle.file_identity) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.rebind",
            L"The rebind bundle identity changed after inspection");
    }
    auto names = impl_->profiles_directory->list_files();
    if (!names.ok()) return Result<ManagedProfileSnapshot>::failure(names.error());
    for (const auto& name : names.value()) {
        const auto profile_id = exact_uuid_leaf(name);
        if (!profile_id.has_value()
            || profile_id.value() == expected.record.profile.profile_uuid) {
            continue;
        }
        auto other = open_profile_store(impl_->trusted_root, profile_id.value());
        if (!other.ok()) return Result<ManagedProfileSnapshot>::failure(other.error());
        auto other_record = other.value().load_current();
        if (!other_record.ok()) {
            if (is_profile_path_not_found(other_record.error())) continue;
            return Result<ManagedProfileSnapshot>::failure(other_record.error());
        }
        if (other_record.value().profile.image_uuid == verified_bundle.image_id
            || equal_ignore_case(
                other_record.value().profile.bundle_path,
                proposed.profile.bundle_path)) {
            return store_error<ManagedProfileSnapshot>(
                L"profile_registration.rebind",
                L"The rebind bundle is already registered");
        }
        auto other_identity = inspect_bundle_identity(
            other_record.value().profile.bundle_path);
        if (!other_identity.ok()) {
            return Result<ManagedProfileSnapshot>::failure(other_identity.error());
        }
        if (other_identity.value() == verified_bundle.file_identity) {
            return store_error<ManagedProfileSnapshot>(
                L"profile_registration.rebind",
                L"The rebind file identity is already registered");
        }
    }
    auto store = open_profile_store(
        impl_->trusted_root, expected.record.profile.profile_uuid);
    if (!store.ok()) return Result<ManagedProfileSnapshot>::failure(store.error());
    auto rebound = store.value().write_rebind(
        expected.record.revision, proposed);
    if (!rebound.ok()) {
        return Result<ManagedProfileSnapshot>::failure(rebound.error());
    }
    auto result = expected;
    result.record = proposed;
    return Result<ManagedProfileSnapshot>::success(std::move(result));
}

Result<ManagedProfileSnapshot>
ProfileRegistrationStore::load_repair_candidate(
    const product::Uuid& profile_id) {
    std::scoped_lock lock(*impl_->gate);
    return repair_candidate_locked(*impl_, profile_id);
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::repair_from_previous(
    const ManagedProfileSnapshot& expected_previous,
    const ProfileState& proposed_state,
    const OperationGateState& proposed_gate) {
    std::scoped_lock lock(*impl_->gate);
    auto candidate = repair_candidate_locked(
        *impl_, expected_previous.record.profile.profile_uuid);
    if (!candidate.ok()) return candidate;
    if (candidate.value() != expected_previous) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.repair",
            L"The expected previous-profile repair candidate is stale");
    }
    auto store = open_profile_store(
        impl_->trusted_root, expected_previous.record.profile.profile_uuid);
    if (!store.ok()) return Result<ManagedProfileSnapshot>::failure(store.error());
    auto current = store.value().load_current();
    if (!current.ok()) {
        if (current.error().code != static_cast<std::uint32_t>(
                profile::ProfileStoreError::current_corrupt)) {
            return Result<ManagedProfileSnapshot>::failure(current.error());
        }
        auto claimed = store.value().claimed_current_revision();
        if (!claimed.ok()) {
            return Result<ManagedProfileSnapshot>::failure(claimed.error());
        }
        auto repaired = store.value().repair_from_previous(
            claimed.value(), *impl_->repair_verifier);
        if (!repaired.ok()) {
            return Result<ManagedProfileSnapshot>::failure(repaired.error());
        }
        current = store.value().load_current();
    }
    if (!current.ok()
        || current.value().revision != expected_previous.record.revision + 1
        || current.value().profile != expected_previous.record.profile) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.repair",
            L"The repaired current profile does not match the verified previous profile");
    }
    auto current_state = load_durable_state(
        impl_->trusted_root, current.value());
    if (!current_state.ok()) {
        return Result<ManagedProfileSnapshot>::failure(current_state.error());
    }
    auto current_snapshot = compose_snapshot(
        current.value(), current_state.value());
    if (!current_snapshot.ok()) return current_snapshot;
    return publish_snapshot_state(
        impl_->trusted_root, current_snapshot.value(), proposed_state,
        proposed_gate, current_snapshot.value().checkpoint,
        current_snapshot.value().compaction, current_snapshot.value().stop);
}

Result<RecoveryCommandReservation>
ProfileRegistrationStore::reserve_recovery_command(
    const ManagedProfileSnapshot& expected,
    const RecoveryCommandReservation& proposed) {
    std::scoped_lock lock(*impl_->gate);
    auto current = proposed.intent.kind == RecoveryOperationKind::repair_profile
        ? repair_candidate_locked(*impl_, proposed.intent.profile_id)
        : load_snapshot(impl_->trusted_root, proposed.intent.profile_id);
    if (!current.ok()) {
        return Result<RecoveryCommandReservation>::failure(current.error());
    }
    if (current.value() != expected || proposed.expected_profile != expected.record
        || proposed.before_state != expected.state
        || proposed.intent.profile_id != expected.record.profile.profile_uuid
        || zero_uuid(proposed.operation_id) || zero_uuid(proposed.request_id)) {
        return store_error<RecoveryCommandReservation>(
            L"profile_registration.recovery_reserve",
            L"The recovery reservation does not match the exact profile snapshot");
    }
    auto existing = find_recovery_locked(*impl_, proposed.operation_id);
    if (!existing.ok()) {
        return Result<RecoveryCommandReservation>::failure(existing.error());
    }
    if (existing.value().has_value()) {
        if (existing.value().value() != proposed) {
            return store_error<RecoveryCommandReservation>(
                L"profile_registration.recovery_reserve",
                L"The recovery operation identifier is already reserved differently");
        }
        return Result<RecoveryCommandReservation>::success(
            existing.value().value());
    }
    auto encoded = encode_recovery_command_reservation(proposed);
    if (!encoded.ok()) {
        return Result<RecoveryCommandReservation>::failure(encoded.error());
    }
    auto published = profile::publish_atomically_with_previous(
        *impl_->profiles_directory,
        recovery_leaf(proposed.operation_id),
        recovery_previous_leaf(proposed.operation_id), encoded.value());
    if (!published.ok()) {
        return Result<RecoveryCommandReservation>::failure(published.error());
    }
    return Result<RecoveryCommandReservation>::success(proposed);
}

Result<std::optional<RecoveryCommandReservation>>
ProfileRegistrationStore::find_recovery_command(
    const product::Uuid& operation_id) {
    std::scoped_lock lock(*impl_->gate);
    return find_recovery_locked(*impl_, operation_id);
}

Result<ForgetTombstone> ProfileRegistrationStore::reserve_forget(
    const ManagedProfileSnapshot& expected,
    const ForgetTombstone& proposed) {
    std::scoped_lock lock(*impl_->gate);
    auto current = load_snapshot(impl_->trusted_root, proposed.profile_id);
    if (!current.ok()) return Result<ForgetTombstone>::failure(current.error());
    if (current.value() != expected
        || proposed.profile_id != expected.record.profile.profile_uuid
        || proposed.image_id != expected.record.profile.image_uuid
        || proposed.profile_revision != expected.record.revision
        || proposed.retained_bundle_path != expected.record.profile.bundle_path
        || proposed.authorization_owner_sid != expected.record.profile.owner_sid
        || proposed.retained_bundle_identity.has_value()
        || proposed.before_state != expected.state
        || proposed.phase != ForgetTombstonePhase::reserved) {
        return store_error<ForgetTombstone>(
            L"profile_registration.forget_reserve",
            L"The forget tombstone does not match the exact profile snapshot");
    }
    auto identity = inspect_bundle_identity(proposed.retained_bundle_path);
    if (!identity.ok()) return Result<ForgetTombstone>::failure(identity.error());
    auto durable = proposed;
    durable.retained_bundle_identity = identity.value();
    auto existing = load_tombstone_locked(*impl_, proposed.operation_id);
    if (!existing.ok()) return Result<ForgetTombstone>::failure(existing.error());
    if (existing.value().has_value()) {
        if (existing.value().value() != durable) {
            return store_error<ForgetTombstone>(
                L"profile_registration.forget_reserve",
                L"The forget operation identifier is already reserved differently");
        }
        return Result<ForgetTombstone>::success(existing.value().value());
    }
    auto published = publish_tombstone_locked(*impl_, durable);
    if (!published.ok()) return Result<ForgetTombstone>::failure(published.error());
    return Result<ForgetTombstone>::success(std::move(durable));
}

Result<std::optional<ForgetTombstone>>
ProfileRegistrationStore::find_forget_tombstone(
    const product::Uuid& operation_id) {
    std::scoped_lock lock(*impl_->gate);
    auto loaded = load_tombstone_locked(*impl_, operation_id);
    if (!loaded.ok() || !loaded.value().has_value()
        || loaded.value()->phase != ForgetTombstonePhase::removal_started) {
        return loaded;
    }
    auto completed = complete_forget_removal_locked(
        *impl_, loaded.value().value());
    if (!completed.ok()) {
        return Result<std::optional<ForgetTombstone>>::failure(
            completed.error());
    }
    return Result<std::optional<ForgetTombstone>>::success(
        completed.move_value());
}

Result<ForgetTombstone> ProfileRegistrationStore::remove_forget_metadata(
    const ForgetMetadataRemovalRequest& request,
    const ForgetSafetyEvidence& proof) {
    std::scoped_lock lock(*impl_->gate);
    auto loaded = load_tombstone_locked(*impl_, request.operation_id);
    if (!loaded.ok()) return Result<ForgetTombstone>::failure(loaded.error());
    if (!loaded.value().has_value()) {
        return store_error<ForgetTombstone>(
            L"profile_registration.forget_remove",
            L"The forget operation has no durable tombstone");
    }
    auto tombstone = loaded.value().value();
    if (tombstone.phase == ForgetTombstonePhase::metadata_removed) {
        return Result<ForgetTombstone>::success(std::move(tombstone));
    }
    if (tombstone.phase == ForgetTombstonePhase::removal_started) {
        return complete_forget_removal_locked(*impl_, std::move(tombstone));
    }
    auto current = load_snapshot(impl_->trusted_root, request.profile_id);
    if (!current.ok()) return Result<ForgetTombstone>::failure(current.error());
    if (request.request_id != tombstone.request_id
        || request.profile_id != tombstone.profile_id
        || request.image_id != tombstone.image_id
        || request.profile_revision != tombstone.profile_revision
        || request.expected_state != current.value().state
        || request.expected_gate != current.value().operation_gate
        || current.value().record.revision != tombstone.profile_revision
        || proof.profile_id != tombstone.profile_id
        || proof.image_id != tombstone.image_id
        || proof.profile_revision != tombstone.profile_revision
        || !proof.worker_absent || !proof.writer_lock_absent
        || !proof.matching_device_absent || !proof.bundle_present
        || proof.retained_bundle_identity
            != tombstone.retained_bundle_identity) {
        return store_error<ForgetTombstone>(
            L"profile_registration.forget_remove",
            L"The metadata-only forget proof or exact profile identity is stale");
    }
    auto retained = inspect_bundle_identity(tombstone.retained_bundle_path);
    if (!retained.ok()) return Result<ForgetTombstone>::failure(retained.error());
    if (!tombstone.retained_bundle_identity.has_value()
        || retained.value() != tombstone.retained_bundle_identity.value()) {
        return store_error<ForgetTombstone>(
            L"profile_registration.forget_remove",
            L"The retained bundle identity changed before metadata removal");
    }
    tombstone.phase = ForgetTombstonePhase::removal_started;
    auto boundary = publish_tombstone_locked(*impl_, tombstone);
    if (!boundary.ok()) return Result<ForgetTombstone>::failure(boundary.error());
    return complete_forget_removal_locked(*impl_, std::move(tombstone));
}

} // namespace myrd::management
