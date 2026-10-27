#include "src/common/diagnostic_trace.h"
#include "src/management/profile_registration_store.h"

#include "src/management/profile_registration_codec.h"
#include "src/profile/profile_store.h"
#include "src/profile/profile_validation.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwctype>
#include <filesystem>
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
        // **Everything below is captured before anything else runs**, because
        // `GetLastError` is thread-local and a single allocation between the call
        // and the read can replace it with an unrelated code.
        const DWORD open_error = GetLastError();
        // A second, identical attempt separates "transient" from "invisible to
        // this process".  The caller has just been told the bundle was created, so
        // "not found" has to be explained by the path or by this process, and the
        // retry is the cheapest way to tell which.
        const HANDLE retry = CreateFileW(
            terminated.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        const DWORD retry_error = GetLastError();
        if (retry != INVALID_HANDLE_VALUE) static_cast<void>(CloseHandle(retry));
        const DWORD attributes = GetFileAttributesW(terminated.c_str());
        const DWORD attributes_error = GetLastError();

        std::wstring detail =
            L"The registered bundle cannot be opened for identity inspection: ";
        detail += terminated;
        if (detail.size() > 300) {
            detail.resize(300);
            detail += L"...";
        }
        detail += L" [pid=";
        detail += std::to_wstring(GetCurrentProcessId());
        detail += L" open_err=";
        detail += std::to_wstring(open_error);
        detail += L" retry_err=";
        detail += std::to_wstring(retry_error);
        detail += L" attr=0x";
        {
            wchar_t hex[16]{};
            static_cast<void>(_snwprintf_s(
                hex, _TRUNCATE, L"%08X",
                static_cast<unsigned>(attributes)));
            detail += hex;
        }
        detail += L" attr_err=";
        detail += std::to_wstring(attributes_error);
        detail += L"]";
        return Result<BundleFileIdentity>::failure(Error{
            ErrorDomain::win32,
            static_cast<std::uint32_t>(ServiceCoreError::dependency_failure),
            L"profile_registration.bundle_identity",
            detail,
            open_error});
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
    // M4 stores its bundle as a directory containing disk.rdm/base/journal;
    // legacy tests and imports may provide a single bundle file.  Both are
    // safe identity anchors when opened without following reparse points.
    if (GetFileType(raw) != FILE_TYPE_DISK
        || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || tag.ReparseTag != 0 || standard.NumberOfLinks != 1) {
        return store_error<BundleFileIdentity>(
            L"profile_registration.bundle_identity",
            L"A registered bundle must be one non-reparse disk object with one link");
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

// A durable record plus its provenance.  Bundling the two means every caller
// that builds a snapshot has to carry the provenance forward explicitly; a
// plain defaulted parameter would let a call site silently claim `durable`.
struct LoadedDurableState final {
    DurableProfileStateRecord record;
    ProfileStateProvenance provenance{ProfileStateProvenance::durable};
};

Result<LoadedDurableState> load_durable_state(
    std::wstring_view root,
    const profile::ProfileRecord& record) {
    auto directory = open_profile_directory(
        root, record.profile.profile_uuid);
    if (!directory.ok()) {
        return Result<LoadedDurableState>::failure(directory.error());
    }
    const auto decode = [](std::span<const std::byte> bytes) {
        return decode_profile_state_record(bytes);
    };
    auto current = read_decoded_durable_file<DurableProfileStateRecord>(
        directory.value(), kStateCurrent, decode);
    if (!current.ok()) {
        return Result<LoadedDurableState>::failure(current.error());
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
                return store_error<LoadedDurableState>(
                    L"profile_registration.load_state",
                    L"Current and previous service states conflict at one revision");
            }
        }
    } else if (!previous.ok()) {
        return Result<LoadedDurableState>::failure(previous.error());
    } else if (previous.value().value.has_value()) {
        selected = previous.value().value.value();
    } else if (current.value().corruption.has_value()) {
        return Result<LoadedDurableState>::failure(
            current.value().corruption.value());
    } else if (previous.value().corruption.has_value()) {
        return Result<LoadedDurableState>::failure(
            previous.value().corruption.value());
    } else {
        // Neither current nor previous service state has ever been written for
        // this profile.  The default keeps M4's load-then-publish bootstrap
        // working, but it is a placeholder rather than an observation, so it
        // is marked as such and never reported to a user as proven state.
        return Result<LoadedDurableState>::success(LoadedDurableState{
            initial_durable_state(record),
            ProfileStateProvenance::synthesized});
    }
    if (!selected->state.lifecycle.has_value()
        || selected->state.lifecycle.value()
            != record.profile.lifecycle) {
        return store_error<LoadedDurableState>(
            L"profile_registration.load_state",
            L"The durable service state does not match the profile lifecycle");
    }
    return Result<LoadedDurableState>::success(
        LoadedDurableState{std::move(*selected),
                           ProfileStateProvenance::durable});
}

Result<ManagedProfileSnapshot> compose_snapshot(
    const profile::ProfileRecord& record,
    LoadedDurableState state) {
    if (!state.record.state.lifecycle.has_value()
        || state.record.state.lifecycle.value() != record.profile.lifecycle) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.compose",
            L"The profile and durable service state disagree");
    }
    return Result<ManagedProfileSnapshot>::success(ManagedProfileSnapshot{
        record,
        state.record.state,
        state.record.operation_gate,
        state.record.checkpoint,
        state.record.compaction,
        state.record.stop,
        state.provenance});
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
    return compose_snapshot(record.value(), state.move_value());
}

Result<void> publish_state_bytes(
    std::wstring_view root,
    const product::Uuid& profile_id,
    const DurableProfileStateRecord& proposed) {
    myrd::diagnostic_trace("regstore.publish_state_bytes: enter");
    if (!validate_durable_profile_state_record(proposed)) {
        return store_error<void>(
            L"profile_registration.publish_state",
            L"The proposed durable service state is semantically contradictory");
    }
    auto encoded = encode_profile_state_record(proposed);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
    myrd::diagnostic_trace("regstore.publish_state_bytes: opening profile directory");
    auto directory = open_profile_directory(root, profile_id);
    myrd::diagnostic_trace("regstore.publish_state_bytes: profile directory open");
    if (!directory.ok()) return Result<void>::failure(directory.error());
    const auto decode = [](std::span<const std::byte> bytes) {
        return decode_profile_state_record(bytes);
    };
    auto current = read_decoded_durable_file<DurableProfileStateRecord>(
        directory.value(), kStateCurrent, decode);
    myrd::diagnostic_trace("regstore.publish_state_bytes: current read returned");
    if (!current.ok()) return Result<void>::failure(current.error());
    bool discard_current = current.value().corruption.has_value();
    auto previous = read_decoded_durable_file<DurableProfileStateRecord>(
        directory.value(), kStatePrevious, decode);
    myrd::diagnostic_trace("regstore.publish_state_bytes: previous read returned");
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
    if (durable.value().record.revision
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.publish",
            L"The durable service-state revision is exhausted");
    }
    DurableProfileStateRecord proposed{
        durable.value().record.revision + 1,
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
    return compose_snapshot(
        expected.record,
        LoadedDurableState{proposed, ProfileStateProvenance::durable});
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
    // **A bundle that is already gone is what this removal is for.**
    //
    // The tombstone's recorded identity is the authority for *what* was removed;
    // re-opening the path can only confirm that it is still the same object.  It
    // used to refuse on a path that no longer opened, which made an interrupted
    // forget -- or any later record recovered after the bundle's directory was
    // deleted -- fail with "the registered bundle cannot be opened", although the
    // removal had in fact already happened.  Fail-closed stays where it belongs:
    // a bundle that *is* there with a different identity is still refused.
    auto retained = inspect_bundle_identity(tombstone.retained_bundle_path);
    if (retained.ok()) {
        if (retained.value() != tombstone.retained_bundle_identity.value()) {
            return store_error<ForgetTombstone>(
                L"profile_registration.forget_complete",
                L"The retained bundle identity changed during metadata removal");
        }
    } else if (retained.error().native_code
                   != static_cast<std::uint32_t>(ERROR_PATH_NOT_FOUND)
               && retained.error().native_code
                   != static_cast<std::uint32_t>(ERROR_FILE_NOT_FOUND)) {
        // Absent is the one failure that means "already removed".  Anything else
        // -- access denied, a device error -- is not evidence of removal.
        return Result<ForgetTombstone>::failure(retained.error());
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
            matching->expected_profile, state.move_value());
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
    state.value().record.state.disk.health =
        product::HealthState::needs_attention;
    return compose_snapshot(previous.value(), state.move_value());
}

} // namespace

// These helpers are defined with the other protected-root policy helpers
// below.  Keep their declarations before the public enumeration methods so
// every entry point applies the same fail-closed policy.
bool allowed_root_metadata_leaf(std::wstring_view name) noexcept;
template <class T>
Result<T> unexpected_profile_entry(const wchar_t* operation);

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
        if (!profile_id.has_value()) {
            if (allowed_root_metadata_leaf(name)) continue;
            return unexpected_profile_entry<ManagedProfileSnapshot>(
                L"profile_registration.resolve");
        }
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

bool allowed_root_metadata_leaf(std::wstring_view name) noexcept {
    // **`workers` is a co-tenant of this root, and it has to be named here.**
    //
    // The worker-fingerprint store keeps its records in `profiles\workers`, by its own
    // argument that they are the same class of state as profile records and should live
    // behind the one protected root the installer creates.  That makes this directory a
    // *shared* root, and this reader the one that has to know it: without this line,
    // `disk list` and `recover_after_restart` both fail with "the protected profiles
    // directory contains an unknown or invalid entry" from the second start onwards --
    // the first start is what creates the leaf (measured on the guest: every start after
    // a start that wrote a fingerprint failed this way).
    //
    // It stays an exact name rather than "ignore directories", because the point of the
    // check is that an unexpected entry in a protected root is a corruption or an attack,
    // and skipping directories wholesale would give up exactly that.  A new co-tenant
    // must be added here by name, which is the moment somebody should have to justify it.
    if (name == L"workers") return true;
    constexpr std::array<std::pair<std::wstring_view, std::wstring_view>, 3> forms{
        std::pair{L"lifecycle-", L".mylr"},
        std::pair{L"recovery-", L".myrr"},
        std::pair{L"forget-", L".myft"}};
    for (const auto& [prefix, suffix] : forms) {
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) continue;
        const auto middle = name.substr(prefix.size(),
            name.size() - prefix.size() - suffix.size());
        if (exact_uuid_leaf(middle).has_value()) return true;
    }
    // The previous copy uses a common suffix but still has the same strict
    // operation-id stem; do not allow arbitrary lookalike files.
    for (const auto prefix : {std::wstring_view(L"lifecycle-"),
                              std::wstring_view(L"recovery-"),
                              std::wstring_view(L"forget-")}) {
        if (!name.starts_with(prefix) || !name.ends_with(L".prev")) continue;
        const auto middle = name.substr(prefix.size(),
            name.size() - prefix.size() - 5);
        if (exact_uuid_leaf(middle).has_value()) return true;
    }
    return false;
}

template <class T>
Result<T> unexpected_profile_entry(const wchar_t* operation) {
    return store_error<T>(operation,
        L"The protected profiles directory contains an unknown or invalid entry");
}

std::wstring lifecycle_leaf(const product::Uuid& operation_id) {
    return L"lifecycle-" + wide_uuid(operation_id) + L".mylr";
}
std::wstring lifecycle_previous_leaf(const product::Uuid& operation_id) {
    return L"lifecycle-" + wide_uuid(operation_id) + L".prev";
}

bool lifecycle_reservation_equal(
    const LifecycleRegistrationReservation& left,
    const LifecycleRegistrationReservation& right) noexcept;

template <class ImplT>
Result<std::optional<LifecycleRegistrationReservation>>
find_lifecycle_locked(ImplT& impl, const product::Uuid& operation_id) {
    const auto decode = [](std::span<const std::byte> bytes) {
        return decode_lifecycle_registration_reservation(bytes);
    };
    auto current = read_decoded_durable_file<LifecycleRegistrationReservation>(
        *impl.profiles_directory, lifecycle_leaf(operation_id), decode);
    if (!current.ok()) return Result<std::optional<LifecycleRegistrationReservation>>::failure(current.error());
    auto previous = read_decoded_durable_file<LifecycleRegistrationReservation>(
        *impl.profiles_directory, lifecycle_previous_leaf(operation_id), decode);
    if (!previous.ok()) return Result<std::optional<LifecycleRegistrationReservation>>::failure(previous.error());
    if (current.value().value.has_value()) {
        if (current.value().value->operation_id != operation_id)
            return store_error<std::optional<LifecycleRegistrationReservation>>(
                L"profile_registration.lifecycle_load", L"The lifecycle reservation filename and identity disagree");
        if (previous.value().value.has_value()) {
            if (previous.value().value->operation_id != operation_id)
                return store_error<std::optional<LifecycleRegistrationReservation>>(
                    L"profile_registration.lifecycle_load", L"The lifecycle reservation filename and identity disagree");
            const auto& current_value = current.value().value.value();
            const auto& previous_value = previous.value().value.value();
            if (previous_value.revision > current_value.revision) {
                return Result<std::optional<LifecycleRegistrationReservation>>::success(
                    previous_value);
            }
            if (previous_value.revision == current_value.revision
                && !lifecycle_reservation_equal(previous_value, current_value)) {
                return store_error<std::optional<LifecycleRegistrationReservation>>(
                    L"profile_registration.lifecycle_load",
                    L"Current and previous lifecycle reservations conflict at one revision");
            }
        }
        return Result<std::optional<LifecycleRegistrationReservation>>::success(
            current.value().value.value());
    }
    if (previous.value().value.has_value()) {
        if (previous.value().value->operation_id != operation_id)
            return store_error<std::optional<LifecycleRegistrationReservation>>(
                L"profile_registration.lifecycle_load", L"The lifecycle reservation filename and identity disagree");
        return Result<std::optional<LifecycleRegistrationReservation>>::success(previous.value().value.value());
    }
    if (current.value().corruption.has_value())
        return Result<std::optional<LifecycleRegistrationReservation>>::failure(current.value().corruption.value());
    if (previous.value().corruption.has_value())
        return Result<std::optional<LifecycleRegistrationReservation>>::failure(previous.value().corruption.value());
    return Result<std::optional<LifecycleRegistrationReservation>>::success(std::nullopt);
}

template <class ImplT>
Result<void> publish_lifecycle_locked(
    ImplT& impl, const LifecycleRegistrationReservation& value) {
    auto encoded = encode_lifecycle_registration_reservation(value);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
    return profile::publish_atomically_with_previous(
        *impl.profiles_directory, lifecycle_leaf(value.operation_id),
        lifecycle_previous_leaf(value.operation_id), encoded.value());
}

bool valid_lifecycle_bundle(
    const LifecycleOperationIntent& intent,
    const BundleRegistrationEvidence& evidence) noexcept;

bool nonzero_volume_evidence(const VolumeRegistrationEvidence& evidence) noexcept {
    return !zero_uuid(evidence.gpt_disk_id)
        && !zero_uuid(evidence.partition_id)
        && !zero_uuid(evidence.volume_id);
}

profile::MountKind registration_mount_kind(std::wstring_view target) noexcept {
    return target.size() == 2 && target[1] == L':'
        ? profile::MountKind::drive_letter : profile::MountKind::directory;
}

Result<profile::ProfileRecord> materialized_registration_profile(
    const LifecycleOperationIntent& intent,
    const BundleRegistrationEvidence& evidence,
    const std::optional<VolumeRegistrationEvidence>& volume) {
    if (!valid_lifecycle_bundle(intent, evidence)) {
        return store_error<profile::ProfileRecord>(
            L"profile_registration.publish",
            L"The lifecycle bundle evidence does not match the operation intent");
    }
    constexpr std::uint64_t alignment = profile::kProfileMemoryAlignmentBytes;
    constexpr std::uint64_t cow_limit = 65ULL * alignment;
    const auto requested = intent.memory_mode == control::MemoryMode::automatic
        ? (std::min)(intent.physical_memory.total_bytes / 4,
                     evidence.logical_capacity_bytes)
        : intent.memory_hard_limit_bytes.value_or(0);
    if (intent.physical_memory.total_bytes == 0
        || intent.physical_memory.available_bytes > intent.physical_memory.total_bytes
        || requested == 0 || requested > evidence.logical_capacity_bytes
        || evidence.logical_capacity_bytes < cow_limit) {
        return store_error<profile::ProfileRecord>(
            L"profile_registration.publish",
            L"The intent memory policy is invalid");
    }
    const auto align_down = [=](std::uint64_t value) {
        return value / alignment * alignment;
    };
    const auto hard = align_down(requested);
    const auto soft = align_down(hard - hard / 10);
    const auto low = align_down(hard - hard / 4);
    if (hard == 0 || !(low < soft && soft < hard)) {
        return store_error<profile::ProfileRecord>(
            L"profile_registration.publish",
            L"The intent memory policy is not materializable");
    }
    profile::CheckpointPolicy checkpoint{};
    if (intent.checkpoint_interval.has_value()) {
        checkpoint.interval_enabled = intent.checkpoint_interval->enabled;
        checkpoint.interval_seconds = static_cast<std::uint64_t>(
            intent.checkpoint_interval->duration.count());
    }
    if (intent.dirty_threshold.has_value()) {
        checkpoint.dirty_threshold_enabled = intent.dirty_threshold->enabled;
        checkpoint.dirty_threshold_bytes = intent.dirty_threshold->bytes;
    } else if (checkpoint.dirty_threshold_bytes > intent.requested_capacity_bytes) {
        // **The default byte threshold cannot exceed the disk it guards.**  T1-10 turned the
        // threshold on by default at 64 MiB, and a 32 MiB disk was then created with a budget
        // larger than its own capacity -- refused by validation, so the create ended with
        // "terminal failure without a worker-action error".  A threshold above the capacity is
        // meaningless rather than harmful, so it is clamped to what the caller asked for.
        checkpoint.dirty_threshold_bytes = intent.requested_capacity_bytes;
    }
    // **The same compaction policy the create's own record carries.**
    //
    // This function rebuilds the profile that a publish must equal, and it used to leave the policy at its default.
    // A create that asked for a lower threshold then failed at `profile_registration.publish` with 22024, "does not
    // exactly match the lifecycle intent and bundle evidence", because the two sides disagreed about a field only one
    // of them set -- and the service log's step for that failure named an unrelated earlier one.
    profile::CompactionPolicy compaction{};
    if (intent.compaction_checkpoint_threshold.has_value()) {
        compaction.checkpoint_count_threshold = *intent.compaction_checkpoint_threshold;
    }
    profile::ProfileRecord expected{
        1,
        profile::Profile{
            intent.profile_id,
            intent.owner_sid,
            intent.display_name,
            intent.created_unix_ms,
            intent.kind == LifecycleOperationKind::create
                ? profile::ProfileLifecycle::provisioning
                : profile::ProfileLifecycle::ready,
            intent.bundle_path,
            evidence.manifest_name,
            evidence.image_id,
            evidence.logical_capacity_bytes,
            evidence.logical_sector_bytes,
            evidence.physical_sector_bytes,
            volume.has_value() ? std::optional<product::Uuid>(volume->gpt_disk_id) : std::nullopt,
            volume.has_value() ? std::optional<product::Uuid>(volume->partition_id) : std::nullopt,
            volume.has_value() ? std::optional<product::Uuid>(volume->volume_id) : std::nullopt,
            registration_mount_kind(intent.mount_target),
            intent.mount_target,
            intent.auto_start,
            profile::MemoryPolicy{
                intent.memory_mode == control::MemoryMode::automatic
                    ? profile::MemoryMode::automatic : profile::MemoryMode::fixed,
                hard, soft, low, cow_limit},
            checkpoint,
            compaction}};
    auto validation = profile::validate_profile(expected);
    if (!validation.ok()) {
        return store_error<profile::ProfileRecord>(
            L"profile_registration.publish",
            L"The intent and bundle evidence do not materialize a valid profile");
    }
    return Result<profile::ProfileRecord>::success(std::move(expected));
}

bool lifecycle_phase_valid_for_intent(
    const LifecycleOperationIntent& intent,
    LifecycleReservationPhase phase) noexcept {
    if (phase > LifecycleReservationPhase::ready_published) return false;
    if (intent.kind == LifecycleOperationKind::create) {
        return phase != LifecycleReservationPhase::inspection_action_started
            && phase != LifecycleReservationPhase::inspection_verified;
    }
    return phase != LifecycleReservationPhase::bundle_action_started
        && phase != LifecycleReservationPhase::bundle_verified;
}

bool lifecycle_reservation_admission_valid(
    const LifecycleRegistrationReservation& value) noexcept {
    if (value.revision == 0 || zero_uuid(value.operation_id)
        || zero_uuid(value.request_id)
        || value.argument_digest == std::array<std::byte, 32>{}
        || zero_uuid(value.intent.profile_id)
        || value.intent.owner_sid.empty()
        || value.intent.display_name.empty()
        || value.intent.bundle_path.empty()
        || value.intent.mount_target.empty()) {
        return false;
    }
    const bool bundle_required =
        value.phase >= LifecycleReservationPhase::bundle_verified;
    const bool volume_required =
        value.phase >= LifecycleReservationPhase::volume_identity_recorded;
    if (!lifecycle_phase_valid_for_intent(value.intent, value.phase)) {
        return false;
    }
    if (!bundle_required && value.bundle.has_value()) return false;
    if (!volume_required && value.volume.has_value()) return false;
    if (bundle_required) {
        if (!value.bundle.has_value()
            || !valid_lifecycle_bundle(value.intent, value.bundle.value())) {
            return false;
        }
    }
    if (volume_required) {
        if (!value.volume.has_value()
            || !nonzero_volume_evidence(value.volume.value())) {
            return false;
        }
    }
    return true;
}

bool lifecycle_intent_equal(
    const LifecycleOperationIntent& left,
    const LifecycleOperationIntent& right) noexcept {
    return left.kind == right.kind && left.profile_id == right.profile_id
        && left.expected_image_id == right.expected_image_id
        && left.owner_sid == right.owner_sid
        && left.display_name == right.display_name
        && left.bundle_path == right.bundle_path
        && left.mount_target == right.mount_target
        && left.requested_capacity_bytes == right.requested_capacity_bytes
        && left.auto_start == right.auto_start
        && left.memory_mode == right.memory_mode
        && left.memory_hard_limit_bytes == right.memory_hard_limit_bytes
        && (!left.checkpoint_interval.has_value() == !right.checkpoint_interval.has_value())
        && (!left.checkpoint_interval.has_value()
            || (left.checkpoint_interval->enabled == right.checkpoint_interval->enabled
                && left.checkpoint_interval->duration == right.checkpoint_interval->duration))
        && (!left.dirty_threshold.has_value() == !right.dirty_threshold.has_value())
        && (!left.dirty_threshold.has_value()
            || (left.dirty_threshold->enabled == right.dirty_threshold->enabled
                && left.dirty_threshold->bytes == right.dirty_threshold->bytes))
        && left.physical_memory.total_bytes == right.physical_memory.total_bytes
        && left.physical_memory.available_bytes == right.physical_memory.available_bytes
        && left.created_unix_ms == right.created_unix_ms;
}

bool lifecycle_reservation_equal(
    const LifecycleRegistrationReservation& left,
    const LifecycleRegistrationReservation& right) noexcept {
    return left.revision == right.revision
        && left.operation_id == right.operation_id
        && left.request_id == right.request_id
        && left.argument_digest == right.argument_digest
        && lifecycle_intent_equal(left.intent, right.intent)
        && left.phase == right.phase && left.bundle == right.bundle
        && left.volume == right.volume;
}

bool reservation_transition_valid(
    const LifecycleRegistrationReservation& expected,
    const LifecycleRegistrationReservation& proposed) noexcept {
    if (proposed.phase < expected.phase
        || !lifecycle_phase_valid_for_intent(
            expected.intent, proposed.phase)) return false;
    if (expected.bundle.has_value()
        && (!proposed.bundle.has_value()
            || proposed.bundle.value() != expected.bundle.value())) return false;
    if (expected.volume.has_value()
        && (!proposed.volume.has_value()
            || proposed.volume.value() != expected.volume.value())) return false;
    return true;
}

bool registration_matches_reservation(
    const LifecycleRegistrationReservation& reservation,
    const ManagedProfileSnapshot& snapshot) noexcept {
    const auto& profile = snapshot.record.profile;
    if (profile.profile_uuid != reservation.intent.profile_id
        || profile.owner_sid != reservation.intent.owner_sid
        || profile.display_name != reservation.intent.display_name
        || profile.bundle_path != reservation.intent.bundle_path
        || profile.mount_target != reservation.intent.mount_target
        || (reservation.intent.requested_capacity_bytes != 0
            && profile.logical_capacity_bytes
                != reservation.intent.requested_capacity_bytes)
        || profile.auto_start != reservation.intent.auto_start
        || !reservation.bundle.has_value()) return false;
    const auto& bundle = reservation.bundle.value();
    if (profile.image_uuid != bundle.image_id
        || profile.manifest_name != bundle.manifest_name
        || profile.logical_capacity_bytes != bundle.logical_capacity_bytes
        || profile.logical_sector_size != bundle.logical_sector_bytes
        || profile.physical_sector_size != bundle.physical_sector_bytes) return false;
    if (reservation.volume.has_value()) {
        const auto& volume = reservation.volume.value();
        if (!profile.gpt_disk_guid.has_value()
            || !profile.partition_guid.has_value()
            || !profile.volume_guid.has_value()
            || profile.gpt_disk_guid.value() != volume.gpt_disk_id
            || profile.partition_guid.value() != volume.partition_id
            || profile.volume_guid.value() != volume.volume_id) return false;
    } else if (profile.gpt_disk_guid.has_value()
        || profile.partition_guid.has_value()
        || profile.volume_guid.has_value()) {
        return false;
    }
    return true;
}

bool valid_lifecycle_bundle(
    const LifecycleOperationIntent& intent,
    const BundleRegistrationEvidence& evidence) noexcept {
    return evidence.source == (intent.kind == LifecycleOperationKind::create
            ? BundleEvidenceSource::created_and_verified
            : BundleEvidenceSource::read_only_inspection)
        && (!intent.expected_image_id.has_value()
            || evidence.image_id == intent.expected_image_id.value())
        && evidence.logical_capacity_bytes != 0
        && (intent.requested_capacity_bytes == 0
            || evidence.logical_capacity_bytes == intent.requested_capacity_bytes)
        && evidence.logical_sector_bytes == kProductLogicalSectorBytes
        && evidence.physical_sector_bytes == kProductPhysicalSectorBytes
        && evidence.payload_block_bytes == kProductPayloadBlockBytes
        && nonzero_file_identity(evidence.file_identity)
        && !evidence.manifest_name.empty();
}

bool same_bundle_registration(
    const BundleRegistrationEvidence& left,
    const BundleRegistrationEvidence& right) noexcept {
    return left == right;
}

std::optional<std::wstring> normalized_mount_target(
    const profile::Profile& value) {
    if (value.mount_kind == profile::MountKind::drive_letter) {
        if (value.mount_target.size() != 2
            || value.mount_target[1] != L':') {
            return std::nullopt;
        }
        std::wstring normalized = value.mount_target;
        normalized[0] = static_cast<wchar_t>(std::towupper(normalized[0]));
        return normalized;
    }
    auto normalized = product::normalize_local_absolute_path(
        value.mount_target);
    if (!normalized.ok()) return std::nullopt;
    return normalized.move_value();
}

bool mount_targets_equal(
    const profile::Profile& left, const profile::Profile& right) {
    if (left.mount_kind != right.mount_kind) return false;
    const auto normalized_left = normalized_mount_target(left);
    const auto normalized_right = normalized_mount_target(right);
    if (!normalized_left.has_value() || !normalized_right.has_value()) {
        // Invalid persisted mount metadata must never be treated as available.
        return true;
    }
    return equal_ignore_case(normalized_left.value(), normalized_right.value());
}

Result<void> ensure_profile_unique(
    std::wstring_view root,
    const profile::ProfileRecord& proposed,
    const BundleRegistrationEvidence& bundle) {
    auto paths = profile::derive_profile_store_paths(
        root, proposed.profile.profile_uuid);
    if (!paths.ok()) return Result<void>::failure(paths.error());
    std::filesystem::path profiles_root(paths.value().profiles_root);
    std::error_code iteration_error;
    std::filesystem::directory_iterator entries(profiles_root, iteration_error);
    if (iteration_error) {
        return store_error<void>(L"profile_registration.publish",
            L"The profile directory cannot be enumerated safely");
    }
    for (const auto& entry : entries) {
        std::error_code entry_error;
        const bool is_directory = entry.is_directory(entry_error);
        if (!is_directory) {
            if (entry_error) {
                return store_error<void>(L"profile_registration.publish",
                    L"A profile directory entry cannot be inspected safely");
            }
            if (allowed_root_metadata_leaf(entry.path().filename().wstring())) continue;
            return unexpected_profile_entry<void>(L"profile_registration.publish");
        }
        auto name = entry.path().filename().wstring();
        // **`workers` is a directory co-tenant of this root**, and the file allowance above
        // only covers files.  Without this line a create reached `publish-provisioning` and
        // then failed with
        //   code=22024 step=profile_registration.publish
        //   detail=The protected profiles directory contains an unknown or invalid entry
        // while `disk list` accepted the very same directory -- the two enumerations differ in
        // exactly this spot.  Named precisely rather than "skip directories": an unexpected
        // entry in a protected root is corruption or an attack, and giving that up to avoid
        // naming one co-tenant would be the wrong trade.
        if (name == L"workers") continue;
        auto id = exact_uuid_leaf(name);
        if (!id.has_value()) {
            return unexpected_profile_entry<void>(L"profile_registration.publish");
        }
        if (id.value() == proposed.profile.profile_uuid) continue;
        auto other = open_profile_store(root, id.value());
        if (!other.ok()) return Result<void>::failure(other.error());
        auto record = other.value().load_current();
        if (!record.ok()) {
            if (is_profile_path_not_found(record.error())) continue;
            return Result<void>::failure(record.error());
        }
        if (equal_ignore_case(record.value().profile.display_name,
                              proposed.profile.display_name)
            || record.value().profile.image_uuid == proposed.profile.image_uuid
            || equal_ignore_case(record.value().profile.bundle_path,
                                 proposed.profile.bundle_path)
            || mount_targets_equal(record.value().profile,
                                   proposed.profile)) {
            return store_error<void>(L"profile_registration.publish",
                L"The profile name, image, or bundle is already registered");
        }
        auto identity = inspect_bundle_identity(record.value().profile.bundle_path);
        if (!identity.ok()) {
            // **An unrelated profile's missing bundle must not block this one.**
            // Identity equality needs both bundles open, so a path that cannot be
            // opened cannot be the bundle being registered -- and the four checks
            // above (name, image, path, mount target) already covered every way
            // the two could actually collide.
            //
            // This used to propagate the raw error, which made one orphaned
            // registration poison the whole service: measured on the guest, a
            // profile left behind with a deleted bundle made every later
            // `disk create` fail with `profile_registration.bundle_identity`
            // naming *the new* bundle, while the missing path belonged to the old
            // profile.
            continue;
        }
        if (identity.value() == bundle.file_identity)
            return store_error<void>(L"profile_registration.publish",
                L"The bundle file identity is already registered");
    }
    return Result<void>::success();
}

Result<std::vector<ManagedProfileSnapshot>>
ProfileRegistrationStore::list_profiles() {
    std::scoped_lock lock(*impl_->gate);
    auto names = impl_->profiles_directory->list_files();
    if (!names.ok()) {
        return Result<std::vector<ManagedProfileSnapshot>>::failure(names.error());
    }
    std::vector<ManagedProfileSnapshot> result;
    for (const auto& name : names.value()) {
        const auto id = exact_uuid_leaf(name);
        if (!id.has_value()) {
            if (allowed_root_metadata_leaf(name)) continue;
            return unexpected_profile_entry<std::vector<ManagedProfileSnapshot>>(
                L"profile_registration.list");
        }
        auto snapshot = load_snapshot(impl_->trusted_root, id.value());
        if (!snapshot.ok()) {
            // A directory containing only a stale runtime lease is not a
            // durable profile.  It remains on disk as a recovery anchor, but
            // must not make disk.list fail or manufacture a managed profile
            // after a facility reset.
            if (snapshot.error().code == static_cast<std::uint32_t>(
                    ServiceCoreError::profile_not_found)
                || is_profile_path_not_found(snapshot.error())
                || snapshot.error().native_code == ERROR_FILE_NOT_FOUND
                || snapshot.error().native_code == ERROR_PATH_NOT_FOUND) {
                continue;
            }
            return Result<std::vector<ManagedProfileSnapshot>>::failure(
                snapshot.error());
        }
        result.push_back(snapshot.move_value());
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.record.profile.profile_uuid.to_string()
            < right.record.profile.profile_uuid.to_string();
    });
    return Result<std::vector<ManagedProfileSnapshot>>::success(std::move(result));
}

Result<ManagedProfileSnapshot> ProfileRegistrationStore::update_profile_record(
    const ManagedProfileSnapshot& expected,
    const profile::ProfileRecord& proposed,
    const ProfileState& state,
    const OperationGateState& gate) {
    std::scoped_lock lock(*impl_->gate);
    auto current = load_snapshot(
        impl_->trusted_root, expected.record.profile.profile_uuid);
    if (!current.ok()) return current;
    if (current.value() != expected) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.update_profile",
            L"The expected profile snapshot is stale");
    }
    const bool running = expected.state.disk.runtime
        == product::RuntimeState::running;
    if (proposed.profile.lifecycle != expected.record.profile.lifecycle) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.update_profile",
            L"The profile lifecycle is owned by lifecycle transitions");
    }
    const auto validation = profile::validate_profile_update(
        expected.record, proposed, running);
    if (!validation.ok()) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.update_profile",
            L"The proposed profile update is invalid");
    }
    if (!state.lifecycle.has_value()
        || state.lifecycle != proposed.profile.lifecycle
        || !is_valid(state)
        || gate.active_operation_id != expected.operation_gate.active_operation_id
        || gate.active_command != expected.operation_gate.active_command) {
        return store_error<ManagedProfileSnapshot>(
            L"profile_registration.update_profile",
            L"The profile update would change runtime ownership");
    }
    auto store = open_profile_store(
        impl_->trusted_root, expected.record.profile.profile_uuid);
    if (!store.ok()) return Result<ManagedProfileSnapshot>::failure(store.error());
    auto written = store.value().write_next(
        expected.record.revision, proposed, running);
    if (!written.ok()) return Result<ManagedProfileSnapshot>::failure(written.error());
    auto result = expected;
    result.record = proposed;
    return Result<ManagedProfileSnapshot>::success(std::move(result));
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
        current.value(), current_state.move_value());
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

Result<LifecycleRegistrationReservation> ProfileRegistrationStore::reserve(
    const LifecycleRegistrationReservation& proposed) {
    std::scoped_lock lock(*impl_->gate);
    if (!lifecycle_reservation_admission_valid(proposed)
        || !validate_lifecycle_registration_reservation(proposed)) {
        return store_error<LifecycleRegistrationReservation>(
            L"profile_registration.lifecycle_reserve",
            L"The lifecycle reservation is incomplete");
    }
    auto existing = find_lifecycle_locked(*impl_, proposed.operation_id);
    if (!existing.ok()) return Result<LifecycleRegistrationReservation>::failure(existing.error());
    if (existing.value().has_value()) {
        if (!lifecycle_reservation_equal(existing.value().value(), proposed)) {
            return store_error<LifecycleRegistrationReservation>(
                L"profile_registration.lifecycle_reserve",
                L"The lifecycle operation identifier is already reserved differently");
        }
        return Result<LifecycleRegistrationReservation>::success(existing.value().value());
    }
    auto published = publish_lifecycle_locked(*impl_, proposed);
    if (!published.ok()) return Result<LifecycleRegistrationReservation>::failure(published.error());
    return Result<LifecycleRegistrationReservation>::success(proposed);
}

Result<LifecycleRegistrationReservation> ProfileRegistrationStore::load_reservation(
    const product::Uuid& operation_id) {
    std::scoped_lock lock(*impl_->gate);
    auto loaded = find_lifecycle_locked(*impl_, operation_id);
    if (!loaded.ok()) return Result<LifecycleRegistrationReservation>::failure(loaded.error());
    if (!loaded.value().has_value()) {
        return store_error<LifecycleRegistrationReservation>(
            L"profile_registration.lifecycle_load",
            L"The lifecycle operation has no durable reservation");
    }
    return Result<LifecycleRegistrationReservation>::success(loaded.move_value().value());
}

Result<LifecycleRegistrationReservation> ProfileRegistrationStore::update_reservation(
    const LifecycleRegistrationReservation& expected,
    const LifecycleRegistrationReservation& proposed) {
    std::scoped_lock lock(*impl_->gate);
    if (!lifecycle_reservation_admission_valid(expected)
        || !lifecycle_reservation_admission_valid(proposed)
        || !validate_lifecycle_registration_reservation(expected)
        || !validate_lifecycle_registration_reservation(proposed)
        || proposed.operation_id != expected.operation_id
        || proposed.request_id != expected.request_id
        || proposed.argument_digest != expected.argument_digest
        || !lifecycle_intent_equal(proposed.intent, expected.intent)
        || !reservation_transition_valid(expected, proposed)
        || expected.revision == (std::numeric_limits<std::uint64_t>::max)()
        || proposed.revision != expected.revision + 1) {
        return store_error<LifecycleRegistrationReservation>(
            L"profile_registration.lifecycle_update",
            L"The lifecycle reservation transition is invalid");
    }
    auto current = find_lifecycle_locked(*impl_, expected.operation_id);
    if (!current.ok()) return Result<LifecycleRegistrationReservation>::failure(current.error());
    if (!current.value().has_value()
        || !lifecycle_reservation_equal(current.value().value(), expected)) {
        return store_error<LifecycleRegistrationReservation>(
            L"profile_registration.lifecycle_update",
            L"The lifecycle reservation revision is stale");
    }
    auto published = publish_lifecycle_locked(*impl_, proposed);
    if (!published.ok()) return Result<LifecycleRegistrationReservation>::failure(published.error());
    return Result<LifecycleRegistrationReservation>::success(proposed);
}

Result<RegistrationMutationResult> ProfileRegistrationStore::publish_registration(
    const LifecycleOperationIntent& intent,
    const profile::ProfileRecord& proposed,
    const BundleRegistrationEvidence& bundle,
    const ProfileState& state,
    const OperationGateState& gate,
    const std::uint64_t bundle_generation) {
    return publish_registration(intent, proposed, bundle, std::nullopt, state,
                                gate, bundle_generation);
}

Result<RegistrationMutationResult> ProfileRegistrationStore::publish_registration(
    const LifecycleOperationIntent& intent,
    const profile::ProfileRecord& proposed,
    const BundleRegistrationEvidence& bundle,
    const std::optional<VolumeRegistrationEvidence>& volume,
    const ProfileState& state,
    const OperationGateState& gate,
    const std::uint64_t bundle_generation) {
    std::scoped_lock lock(*impl_->gate);
    auto observed_identity = inspect_bundle_identity(intent.bundle_path);
    if (!observed_identity.ok() || observed_identity.value() != bundle.file_identity) {
        return store_error<RegistrationMutationResult>(
            L"profile_registration.publish",
            L"The bundle file identity is stale or forged");
    }
    if (volume.has_value() && !nonzero_volume_evidence(volume.value())) {
        return store_error<RegistrationMutationResult>(
            L"profile_registration.publish",
            L"The volume identity evidence is invalid");
    }
    auto materialized = materialized_registration_profile(intent, bundle, volume);
    if (!materialized.ok()
        || proposed != materialized.value()
        || !profile::validate_profile(proposed).ok()) {
        return store_error<RegistrationMutationResult>(
            L"profile_registration.publish",
            L"The proposed registration does not exactly match the lifecycle intent and bundle evidence");
    }
    auto existing = load_snapshot(impl_->trusted_root, intent.profile_id);
    if (existing.ok()) {
        if (existing.value().record != proposed
            || existing.value().state != state
            || existing.value().operation_gate != gate) {
            return store_error<RegistrationMutationResult>(
                L"profile_registration.publish",
                L"The profile identifier is already registered with different resources or lifecycle metadata");
        }
        return Result<RegistrationMutationResult>::success({
            RegistrationCommitDisposition::committed, existing.move_value()});
    }
    if (existing.error().code != static_cast<std::uint32_t>(ServiceCoreError::profile_not_found)) {
        return Result<RegistrationMutationResult>::failure(existing.error());
    }
    auto unique = ensure_profile_unique(impl_->trusted_root, proposed, bundle);
    if (!unique.ok()) return Result<RegistrationMutationResult>::failure(unique.error());
    auto created = profile::create_profile_directory(
        impl_->trusted_root, intent.profile_id);
    if (!created.ok()) return Result<RegistrationMutationResult>::failure(created.error());
    auto store = open_profile_store(impl_->trusted_root, intent.profile_id);
    if (!store.ok()) return Result<RegistrationMutationResult>::failure(store.error());
    auto written = store.value().write_next(0, proposed, false);
    if (!written.ok()) return Result<RegistrationMutationResult>::failure(written.error());
    // **The durable generation starts where the bundle already is**, not at zero.
    //
    // A bundle that has just been created already stands at a committed generation (measured
    // on the guest: 1).  Publishing 0 here makes the create's first checkpoint ask to advance
    // 1 -> 2 while the record says confirmed = 0, which `valid_checkpoint_boundary` correctly
    // refuses -- so every create failed one step after provisioning, with the refusal naming a
    // correlation rather than a generation.
    //
    // The number comes from the worker (the only layer allowed to read a bundle) and is passed
    // in; a caller that does not know it passes 0, which is what a brand-new empty bundle
    // legitimately stands at.
    CheckpointDurableState initial_checkpoint{};
    initial_checkpoint.confirmed_generation = bundle_generation;
    DurableProfileStateRecord durable{
        1, state, gate, initial_checkpoint,
        CompactionDurableState{}, StopDurableState{}};
    auto published = publish_state_bytes(
        impl_->trusted_root, intent.profile_id, durable);
    if (!published.ok()) {
        // The cause travels with the verdict.  `indeterminate` alone told whoever read the
        // log that the durable state *might* have landed, and nothing about which write or
        // why -- which is exactly the question a create that fails here leaves open.
        return Result<RegistrationMutationResult>::success(
            {RegistrationCommitDisposition::indeterminate, std::nullopt,
             published.error()});
    }
    auto snapshot = compose_snapshot(
        proposed,
        LoadedDurableState{durable, ProfileStateProvenance::durable});
    if (!snapshot.ok()) return Result<RegistrationMutationResult>::failure(snapshot.error());
    return Result<RegistrationMutationResult>::success({
        RegistrationCommitDisposition::committed, snapshot.move_value()});
}

Result<RegistrationMutationResult> ProfileRegistrationStore::update_registration(
    const ManagedProfileSnapshot& expected,
    const profile::ProfileRecord& proposed,
    const ProfileState& state,
    const OperationGateState& gate) {
    std::scoped_lock lock(*impl_->gate);
    auto current = load_snapshot(
        impl_->trusted_root, expected.record.profile.profile_uuid);
    if (!current.ok()) return Result<RegistrationMutationResult>::failure(current.error());
    if (current.value() != expected || proposed.revision <= expected.record.revision
        || !profile::validate_profile_update(
            expected.record, proposed,
            expected.state.disk.runtime == product::RuntimeState::running).ok()) {
        return store_error<RegistrationMutationResult>(
            L"profile_registration.update",
            L"The registration update is stale or invalid");
    }
    auto store = open_profile_store(
        impl_->trusted_root, expected.record.profile.profile_uuid);
    if (!store.ok()) return Result<RegistrationMutationResult>::failure(store.error());
    auto written = store.value().write_next(
        expected.record.revision, proposed,
        expected.state.disk.runtime == product::RuntimeState::running);
    if (!written.ok()) {
        if (written.error().code == static_cast<std::uint32_t>(profile::ProfileStoreError::commit_indeterminate)) {
            return Result<RegistrationMutationResult>::success({
                RegistrationCommitDisposition::indeterminate, std::nullopt});
        }
        return Result<RegistrationMutationResult>::failure(written.error());
    }
    auto durable = load_durable_state(
        impl_->trusted_root, expected.record);
    if (!durable.ok()) {
        return Result<RegistrationMutationResult>::success({
            RegistrationCommitDisposition::indeterminate, std::nullopt});
    }
    if (durable.value().record.revision
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<RegistrationMutationResult>::success({
            RegistrationCommitDisposition::indeterminate, std::nullopt});
    }
    DurableProfileStateRecord proposed_state{
        durable.value().record.revision + 1, state, gate,
        durable.value().record.checkpoint, durable.value().record.compaction,
        durable.value().record.stop};
    auto state_written = publish_state_bytes(
        impl_->trusted_root, proposed.profile.profile_uuid, proposed_state);
    if (!state_written.ok()) {
        return Result<RegistrationMutationResult>::success({
            RegistrationCommitDisposition::indeterminate, std::nullopt});
    }
    auto result = ManagedProfileSnapshot{
        proposed, state, gate, proposed_state.checkpoint,
        proposed_state.compaction, proposed_state.stop};
    return Result<RegistrationMutationResult>::success({
        RegistrationCommitDisposition::committed, std::move(result)});
}

Result<RegistrationResolution> ProfileRegistrationStore::resolve_registration(
    const product::Uuid& operation_id,
    const product::Uuid& profile_id) {
    std::scoped_lock lock(*impl_->gate);
    auto reservation = find_lifecycle_locked(*impl_, operation_id);
    if (!reservation.ok()) return Result<RegistrationResolution>::failure(reservation.error());
    if (!reservation.value().has_value()
        || reservation.value()->intent.profile_id != profile_id) {
        return Result<RegistrationResolution>::success({
            RegistrationResolutionDisposition::ambiguous, std::nullopt});
    }
    auto snapshot = load_snapshot(impl_->trusted_root, profile_id);
    if (!snapshot.ok()) {
        if (snapshot.error().code == static_cast<std::uint32_t>(ServiceCoreError::profile_not_found)) {
            return Result<RegistrationResolution>::success({
                RegistrationResolutionDisposition::absent, std::nullopt});
        }
        return Result<RegistrationResolution>::failure(snapshot.error());
    }
    if (reservation.value()->phase < LifecycleReservationPhase::profile_published) {
        return Result<RegistrationResolution>::success({
            RegistrationResolutionDisposition::ambiguous, std::nullopt});
    }
    if (!registration_matches_reservation(
            reservation.value().value(), snapshot.value())) {
        return Result<RegistrationResolution>::success({
            RegistrationResolutionDisposition::ambiguous, std::nullopt});
    }
    return Result<RegistrationResolution>::success({
        RegistrationResolutionDisposition::exact, snapshot.move_value()});
}

} // namespace myrd::management
