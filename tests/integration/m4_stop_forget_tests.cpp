#include "tests/support/test_framework.h"

#include "src/management/profile_registration_codec.h"
#include "src/management/profile_registration_store.h"
#include "src/profile/profile_store.h"

#define MYRD_CHECKPOINT_FIXTURE_ONLY
#include "../model/m4_checkpoint_operation_tests.cpp"

#define NOMINMAX
#include <Windows.h>
#include <Sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {

class TempBundle final {
public:
    TempBundle() {
        const auto token = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        root_ = std::filesystem::temp_directory_path()
            / ("myrd-m4-forget-" + std::to_string(token));
        std::filesystem::create_directories(root_);
        path_ = root_ / "retained.myrd";
    }

    ~TempBundle() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

class NativeStoreSandbox final {
public:
    explicit NativeStoreSandbox(const product::Uuid& profile_id) {
        static std::atomic_uint32_t sequence{};
        root_ = std::filesystem::temp_directory_path()
            / (L"myrd-m4-native-registration-"
               + std::to_wstring(GetCurrentProcessId()) + L"-"
               + std::to_wstring(sequence.fetch_add(1)));
        REQUIRE(std::filesystem::create_directories(program_data()));
        create_secure_directory(program_data() / L"MyRamDisk");
        create_secure_directory(program_data() / L"MyRamDisk" / L"profiles");
        const auto text = profile_id.to_string();
        create_secure_directory(
            program_data() / L"MyRamDisk" / L"profiles"
            / std::wstring(text.begin(), text.end()));
    }

    ~NativeStoreSandbox() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    std::filesystem::path program_data() const {
        return root_ / L"ProgramData";
    }

private:
    static void create_secure_directory(const std::filesystem::path& path) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
        const auto error = created ? ERROR_SUCCESS : GetLastError();
        LocalFree(descriptor);
        REQUIRE(created || error == ERROR_ALREADY_EXISTS);
    }

    std::filesystem::path root_;
};

class AcceptingRepairVerifier final
    : public profile::IProfileRepairBindingVerifier {
public:
    Result<void> verify_repair_binding(
        const profile::ProfileRecord&) override {
        return Result<void>::success();
    }
};

void write_bytes(
    const std::filesystem::path& path,
    const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    const auto length = std::filesystem::file_size(path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    REQUIRE(input.good() || input.eof());
    REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

BundleFileIdentity file_identity(const std::filesystem::path& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    FILE_ID_INFO native{};
    REQUIRE(GetFileInformationByHandleEx(
        handle, FileIdInfo, &native, sizeof(native)));
    REQUIRE(CloseHandle(handle));
    BundleFileIdentity result{};
    result.volume_serial = native.VolumeSerialNumber;
    std::transform(
        std::begin(native.FileId.Identifier),
        std::end(native.FileId.Identifier), result.file_id.begin(),
        [](unsigned char value) { return static_cast<std::byte>(value); });
    return result;
}

std::wstring uuid_wide(const product::Uuid& value) {
    const auto text = value.to_string();
    return std::wstring(text.begin(), text.end());
}

void stop_fixture(Fixture& fixture) {
    auto snapshot = fixture.profiles.snapshot();
    snapshot.state = registered_profile_state(
        profile::ProfileLifecycle::ready,
        product::DiskState{
            product::RuntimeState::stopped,
            product::ActivityState::idle,
            product::PersistenceState::clean,
            product::HealthState::healthy});
    snapshot.operation_gate = idle_operation_gate();
    fixture.profiles.replace(std::move(snapshot));
}

} // namespace

TEST_CASE(forget_preserves_retained_bundle_byte_for_byte) {
    TempBundle bundle;
    const std::vector<std::byte> sentinel{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x7f}, std::byte{0x80},
        std::byte{0xfe}, std::byte{0xff}, std::byte{'M'}, std::byte{'Y'},
        std::byte{'R'}, std::byte{'D'}, std::byte{0x00}, std::byte{0xa5}};
    write_bytes(bundle.path(), sentinel);
    const auto before_bytes = read_bytes(bundle.path());

    Fixture fixture(product::PersistenceState::clean);
    stop_fixture(fixture);
    auto snapshot = fixture.profiles.snapshot();
    snapshot.record.profile.bundle_path = bundle.path().wstring();
    fixture.profiles.replace(std::move(snapshot));
    const auto profile_id =
        fixture.profiles.snapshot().record.profile.profile_uuid;
    auto identity = caller(
        fixture.profiles.snapshot().record.profile.owner_sid);
    identity.administrator = true;
    identity.elevated = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    const ServiceCommandRequest request{
        uuid("eeeeeee1-eeee-4eee-8eee-eeeeeeeeeee1"),
        control::DiskForgetCommand{profile_id, profile_id},
        identity,
        true,
        false,
        std::nullopt,
        ClientWaitState::active};

    auto queued = core.dispatch(request);
    REQUIRE(queued.ok());
    auto completed = core.execute_operation(
        queued.value().operation_id.value());

    REQUIRE(completed.ok());
    REQUIRE_EQ(completed.value().state, OperationState::succeeded);
    REQUIRE(fixture.profiles.forgotten);
    REQUIRE(std::filesystem::exists(bundle.path()));
    REQUIRE_EQ(read_bytes(bundle.path()), before_bytes);
}

TEST_CASE(native_registration_store_persists_stop_and_forgets_only_metadata) {
    TempBundle bundle;
    TempBundle rebound_bundle;
    const std::vector<std::byte> sentinel{
        std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'}};
    write_bytes(bundle.path(), sentinel);
    const std::vector<std::byte> rebound_sentinel{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
        std::byte{'R'}, std::byte{'E'}, std::byte{'B'}, std::byte{'D'}};
    write_bytes(rebound_bundle.path(), rebound_sentinel);

    auto record = running_profile(product::PersistenceState::clean).record;
    record.profile.bundle_path = bundle.path().wstring();
    const auto profile_id = record.profile.profile_uuid;
    NativeStoreSandbox sandbox(profile_id);
    auto profile_store = profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(profile_store.ok());
    REQUIRE(profile_store.value().write_next(0, record, false).ok());

    AcceptingRepairVerifier repair_verifier;
    auto native_store = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), repair_verifier);
    REQUIRE(native_store.ok());
    auto bootstrap = native_store.value().load(profile_id);
    REQUIRE(bootstrap.ok());
    auto running = bootstrap.value();
    running.state = registered_profile_state(
        profile::ProfileLifecycle::ready,
        product::DiskState{
            product::RuntimeState::running,
            product::ActivityState::idle,
            product::PersistenceState::clean,
            product::HealthState::healthy});
    auto checkpoint = running_profile(product::PersistenceState::clean).checkpoint;
    auto published = native_store.value().publish_checkpoint(
        bootstrap.value(), running.state, running.operation_gate, checkpoint);
    REQUIRE(published.ok());

    Fixture stop_fixture(product::PersistenceState::clean);
    ServiceCore stop_core(
        stop_fixture.clock, native_store.value(), stop_fixture.operations,
        stop_fixture.worker, stop_fixture.logger, stop_fixture.ids,
        stop_fixture.memory);
    const ServiceCommandRequest stop_request{
        uuid("eeeeeee2-eeee-4eee-8eee-eeeeeeeeeee2"),
        control::DiskStopCommand{record.profile.display_name, false, false},
        caller(record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
    auto stop_queued = stop_core.dispatch(stop_request);
    REQUIRE(stop_queued.ok());
    auto stopped = stop_core.execute_operation(
        stop_queued.value().operation_id.value());
    REQUIRE(stopped.ok());
    REQUIRE_EQ(stopped.value().state, OperationState::succeeded);

    auto reopened = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), repair_verifier);
    REQUIRE(reopened.ok());
    auto stopped_snapshot = reopened.value().load(profile_id);
    REQUIRE(stopped_snapshot.ok());
    REQUIRE_EQ(
        stopped_snapshot.value().state.disk.runtime,
        product::RuntimeState::stopped);

    Fixture rebind_fixture(product::PersistenceState::clean);
    rebind_fixture.worker.rebind_evidence = BundleRegistrationEvidence{
        record.profile.image_uuid,
        record.profile.manifest_name,
        record.profile.logical_capacity_bytes,
        record.profile.logical_sector_size,
        record.profile.physical_sector_size,
        kProductPayloadBlockBytes,
        file_identity(rebound_bundle.path()),
        BundleEvidenceSource::read_only_inspection};
    ServiceCore rebind_core(
        rebind_fixture.clock, reopened.value(), rebind_fixture.operations,
        rebind_fixture.worker, rebind_fixture.logger, rebind_fixture.ids,
        rebind_fixture.memory);
    auto rebind_administrator = caller(record.profile.owner_sid);
    rebind_administrator.administrator = true;
    rebind_administrator.elevated = true;
    const ServiceCommandRequest rebind_request{
        uuid("eeeeeee4-eeee-4eee-8eee-eeeeeeeeeee4"),
        control::DiskRebindCommand{
            profile_id, rebound_bundle.path().wstring(),
            record.profile.image_uuid},
        rebind_administrator,
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
    auto rebind_queued = rebind_core.dispatch(rebind_request);
    REQUIRE(rebind_queued.ok());
    auto rebound = rebind_core.execute_operation(
        rebind_queued.value().operation_id.value());
    REQUIRE(rebound.ok());
    REQUIRE_EQ(rebound.value().state, OperationState::succeeded);
    auto rebound_snapshot = reopened.value().load(profile_id);
    REQUIRE(rebound_snapshot.ok());
    REQUIRE_EQ(
        rebound_snapshot.value().record.profile.bundle_path,
        rebound_bundle.path().wstring());

    Fixture forget_fixture(product::PersistenceState::clean);
    ServiceCore forget_core(
        forget_fixture.clock, reopened.value(), forget_fixture.operations,
        forget_fixture.worker, forget_fixture.logger, forget_fixture.ids,
        forget_fixture.memory);
    auto administrator = caller(record.profile.owner_sid);
    administrator.administrator = true;
    administrator.elevated = true;
    const ServiceCommandRequest forget_request{
        uuid("eeeeeee3-eeee-4eee-8eee-eeeeeeeeeee3"),
        control::DiskForgetCommand{profile_id, profile_id},
        administrator,
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
    auto forget_queued = forget_core.dispatch(forget_request);
    REQUIRE(forget_queued.ok());
    auto tombstone = reopened.value().find_forget_tombstone(
        forget_queued.value().operation_id.value());
    REQUIRE(tombstone.ok());
    REQUIRE(tombstone.value().has_value());
    REQUIRE(tombstone.value()->retained_bundle_identity.has_value());
    forget_fixture.worker.forget_proof = ForgetSafetyEvidence{
        profile_id,
        record.profile.image_uuid,
        rebound_snapshot.value().record.revision,
        true,
        true,
        true,
        true,
        tombstone.value()->retained_bundle_identity};
    auto forgotten = forget_core.execute_operation(
        forget_queued.value().operation_id.value());
    REQUIRE(forgotten.ok());
    REQUIRE_EQ(forgotten.value().state, OperationState::succeeded);
    REQUIRE(!reopened.value().load(profile_id).ok());
    REQUIRE(std::filesystem::exists(bundle.path()));
    REQUIRE_EQ(read_bytes(bundle.path()), sentinel);
    REQUIRE(std::filesystem::exists(rebound_bundle.path()));
    REQUIRE_EQ(read_bytes(rebound_bundle.path()), rebound_sentinel);
}

TEST_CASE(native_registration_store_serializes_independent_snapshot_cas) {
    TempBundle bundle;
    write_bytes(bundle.path(), {std::byte{0x01}});
    auto record = running_profile(product::PersistenceState::clean).record;
    record.profile.bundle_path = bundle.path().wstring();
    NativeStoreSandbox sandbox(record.profile.profile_uuid);
    auto profile_store = profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), record.profile.profile_uuid);
    REQUIRE(profile_store.ok());
    REQUIRE(profile_store.value().write_next(0, record, false).ok());
    AcceptingRepairVerifier verifier;
    auto first = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    auto second = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    const auto expected = first.value().load(record.profile.profile_uuid);
    REQUIRE(expected.ok());

    std::atomic_uint32_t ready{};
    std::atomic_bool start{};
    std::atomic_uint32_t successes{};
    const auto publish = [&](ProfileRegistrationStore& store,
                             product::HealthState health) {
        ++ready;
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        auto state = expected.value().state;
        state.disk.health = health;
        if (store.publish_runtime(
                expected.value(), state,
                expected.value().operation_gate).ok()) {
            ++successes;
        }
    };
    std::thread a(
        publish, std::ref(first.value()), product::HealthState::degraded);
    std::thread b(
        publish, std::ref(second.value()),
        product::HealthState::needs_attention);
    while (ready.load(std::memory_order_acquire) != 2U) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    a.join();
    b.join();
    REQUIRE_EQ(successes.load(), 1U);
}

TEST_CASE(native_registration_store_rejects_directory_as_retained_bundle) {
    TempBundle bundle;
    REQUIRE(std::filesystem::create_directories(bundle.path()));
    auto record = running_profile(product::PersistenceState::clean).record;
    record.profile.bundle_path = bundle.path().wstring();
    NativeStoreSandbox sandbox(record.profile.profile_uuid);
    auto profile_store = profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), record.profile.profile_uuid);
    REQUIRE(profile_store.ok());
    REQUIRE(profile_store.value().write_next(0, record, false).ok());
    AcceptingRepairVerifier verifier;
    auto registrations = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(registrations.ok());
    Fixture fixture(product::PersistenceState::clean);
    auto administrator = caller(record.profile.owner_sid);
    administrator.administrator = true;
    administrator.elevated = true;
    ServiceCore core(
        fixture.clock, registrations.value(), fixture.operations,
        fixture.worker, fixture.logger, fixture.ids, fixture.memory);
    const ServiceCommandRequest request{
        uuid("eeeeeee7-eeee-4eee-8eee-eeeeeeeeeee7"),
        control::DiskForgetCommand{
            record.profile.profile_uuid, record.profile.profile_uuid},
        administrator,
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
    REQUIRE(!core.dispatch(request).ok());
    REQUIRE(registrations.value().load(record.profile.profile_uuid).ok());
    REQUIRE(std::filesystem::is_directory(bundle.path()));
}

TEST_CASE(native_registration_store_rejects_hard_linked_retained_bundle) {
    TempBundle bundle;
    write_bytes(bundle.path(), {std::byte{0x21}});
    const auto alias = bundle.path().parent_path() / L"alias.myrd";
    REQUIRE(CreateHardLinkW(alias.c_str(), bundle.path().c_str(), nullptr));
    auto record = running_profile(product::PersistenceState::clean).record;
    record.profile.bundle_path = bundle.path().wstring();
    NativeStoreSandbox sandbox(record.profile.profile_uuid);
    auto profile_store = profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), record.profile.profile_uuid);
    REQUIRE(profile_store.ok());
    REQUIRE(profile_store.value().write_next(0, record, false).ok());
    AcceptingRepairVerifier verifier;
    auto registrations = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(registrations.ok());
    Fixture fixture(product::PersistenceState::clean);
    auto administrator = caller(record.profile.owner_sid);
    administrator.administrator = true;
    administrator.elevated = true;
    ServiceCore core(
        fixture.clock, registrations.value(), fixture.operations,
        fixture.worker, fixture.logger, fixture.ids, fixture.memory);
    const ServiceCommandRequest request{
        uuid("eeeeeeea-eeee-4eee-8eee-eeeeeeeeeeea"),
        control::DiskForgetCommand{
            record.profile.profile_uuid, record.profile.profile_uuid},
        administrator,
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
    REQUIRE(!core.dispatch(request).ok());
    REQUIRE(registrations.value().load(record.profile.profile_uuid).ok());
    REQUIRE_EQ(read_bytes(alias), std::vector<std::byte>{std::byte{0x21}});
}

TEST_CASE(native_registration_store_recovers_service_state_from_previous) {
    TempBundle bundle;
    write_bytes(bundle.path(), {std::byte{0x31}});
    auto record = running_profile(product::PersistenceState::clean).record;
    record.profile.bundle_path = bundle.path().wstring();
    NativeStoreSandbox sandbox(record.profile.profile_uuid);
    auto profile_store = profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), record.profile.profile_uuid);
    REQUIRE(profile_store.ok());
    REQUIRE(profile_store.value().write_next(0, record, false).ok());
    AcceptingRepairVerifier verifier;
    auto registrations = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(registrations.ok());
    auto initial = registrations.value().load(record.profile.profile_uuid);
    REQUIRE(initial.ok());
    auto degraded_state = initial.value().state;
    degraded_state.disk.health = product::HealthState::degraded;
    auto degraded = registrations.value().publish_runtime(
        initial.value(), degraded_state, initial.value().operation_gate);
    REQUIRE(degraded.ok());
    auto attention_state = degraded.value().state;
    attention_state.disk.health = product::HealthState::needs_attention;
    REQUIRE(registrations.value().publish_runtime(
        degraded.value(), attention_state,
        degraded.value().operation_gate).ok());

    const auto id = record.profile.profile_uuid.to_string();
    const auto profile_root = sandbox.program_data() / L"MyRamDisk" / L"profiles"
        / std::wstring(id.begin(), id.end());
    const auto current_state_path = profile_root / L"service_state.myrs";
    const auto supported_current = read_bytes(current_state_path);
    REQUIRE(supported_current.size() > 10U);
    auto unsupported_current = supported_current;
    unsupported_current[8] = std::byte{0x02};
    unsupported_current[9] = std::byte{0x00};
    write_bytes(current_state_path, unsupported_current);
    auto downgrade_store = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(downgrade_store.ok());
    auto downgrade_blocked = downgrade_store.value().load(
        record.profile.profile_uuid);
    REQUIRE(!downgrade_blocked.ok());
    REQUIRE_EQ(
        downgrade_blocked.error().code,
        static_cast<std::uint32_t>(
            ProfileRegistrationCodecError::unsupported_schema));
    write_bytes(current_state_path, supported_current);

    unsupported_current = supported_current;
    unsupported_current[10] = std::byte{0x01};
    unsupported_current[11] = std::byte{0x00};
    write_bytes(current_state_path, unsupported_current);
    auto minor_store = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(minor_store.ok());
    auto minor_blocked = minor_store.value().load(
        record.profile.profile_uuid);
    REQUIRE(!minor_blocked.ok());
    REQUIRE_EQ(
        minor_blocked.error().code,
        static_cast<std::uint32_t>(
            ProfileRegistrationCodecError::unsupported_schema));
    write_bytes(current_state_path, supported_current);

    const HANDLE exclusive = CreateFileW(
        current_state_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(exclusive != INVALID_HANDLE_VALUE);
    auto blocked_store = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(blocked_store.ok());
    auto blocked = blocked_store.value().load(record.profile.profile_uuid);
    REQUIRE(!blocked.ok());
    REQUIRE_EQ(blocked.error().native_code,
               static_cast<std::uint32_t>(ERROR_SHARING_VIOLATION));
    REQUIRE(CloseHandle(exclusive));

    auto corrupt = read_bytes(current_state_path);
    REQUIRE(!corrupt.empty());
    corrupt.back() ^= std::byte{0x80};
    write_bytes(current_state_path, corrupt);

    auto reopened = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(reopened.ok());
    auto recovered = reopened.value().load(record.profile.profile_uuid);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value().state, degraded_state);
    auto republished_state = recovered.value().state;
    republished_state.disk.health = product::HealthState::healthy;
    REQUIRE(reopened.value().publish_runtime(
        recovered.value(), republished_state,
        recovered.value().operation_gate).ok());
    auto preserved_previous = decode_profile_state_record(
        read_bytes(profile_root / L"service_state.prev"));
    REQUIRE(preserved_previous.ok());
    REQUIRE_EQ(preserved_previous.value().state, degraded_state);
}

TEST_CASE(native_registration_store_recovers_reservation_from_previous) {
    TempBundle bundle;
    write_bytes(bundle.path(), {std::byte{0x41}});
    auto record = running_profile(product::PersistenceState::clean).record;
    record.profile.bundle_path = bundle.path().wstring();
    NativeStoreSandbox sandbox(record.profile.profile_uuid);
    auto profile_store = profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), record.profile.profile_uuid);
    REQUIRE(profile_store.ok());
    REQUIRE(profile_store.value().write_next(0, record, false).ok());
    AcceptingRepairVerifier verifier;
    auto registrations = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(registrations.ok());
    auto expected = registrations.value().load(record.profile.profile_uuid);
    REQUIRE(expected.ok());
    const auto operation_id =
        uuid("eeeeeee8-eeee-4eee-8eee-eeeeeeeeeee8");
    const RecoveryCommandReservation proposed{
        operation_id,
        uuid("eeeeeee9-eeee-4eee-8eee-eeeeeeeeeee9"),
        RecoveryOperationIntent{
            RecoveryOperationKind::rebind,
            record.profile.profile_uuid,
            bundle.path().wstring(),
            record.profile.image_uuid,
            std::nullopt,
            false},
        expected.value().state,
        expected.value().record};
    auto reserved = registrations.value().reserve_recovery_command(
        expected.value(), proposed);
    REQUIRE(reserved.ok());
    REQUIRE_EQ(reserved.value(), proposed);

    auto encoded = encode_recovery_command_reservation(proposed);
    REQUIRE(encoded.ok());
    constexpr std::array<std::wstring_view, 2> components{
        L"MyRamDisk", L"profiles"};
    auto root = profile::open_managed_atomic_directory(
        sandbox.program_data().wstring(), components);
    REQUIRE(root.ok());
    const auto operation_text = uuid_wide(operation_id);
    REQUIRE(profile::publish_atomically_with_previous(
        *root.value().directory,
        L"recovery-" + operation_text + L".myrr",
        L"recovery-" + operation_text + L".prev",
        encoded.value()).ok());
    REQUIRE(root.value().directory->remove_file(
        L"recovery-" + operation_text + L".myrr").ok());

    auto reopened = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(reopened.ok());
    auto recovered = reopened.value().find_recovery_command(operation_id);
    REQUIRE(recovered.ok());
    REQUIRE(recovered.value().has_value());
    REQUIRE_EQ(recovered.value().value(), proposed);
}

TEST_CASE(native_registration_store_repairs_corrupt_current_through_profile_store) {
    TempBundle bundle;
    const std::vector<std::byte> sentinel{
        std::byte{0x91}, std::byte{0x92}, std::byte{0x93}};
    write_bytes(bundle.path(), sentinel);
    auto first_record = running_profile(product::PersistenceState::clean).record;
    first_record.revision = 1;
    first_record.profile.bundle_path = bundle.path().wstring();
    NativeStoreSandbox sandbox(first_record.profile.profile_uuid);
    auto profile_store = profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), first_record.profile.profile_uuid);
    REQUIRE(profile_store.ok());
    REQUIRE(profile_store.value().write_next(0, first_record, false).ok());
    auto second_record = first_record;
    second_record.revision = 2;
    second_record.profile.display_name = L"corrupt-current";
    REQUIRE(profile_store.value().write_next(1, second_record, false).ok());
    auto corrupt = read_bytes(profile_store.value().paths().current_profile);
    REQUIRE(corrupt.size() > profile::kProfileHeaderBytes);
    corrupt.back() ^= std::byte{0x80};
    write_bytes(profile_store.value().paths().current_profile, corrupt);

    AcceptingRepairVerifier verifier;
    auto registrations = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(registrations.ok());
    Fixture fixture(product::PersistenceState::clean);
    auto administrator = caller(first_record.profile.owner_sid);
    administrator.administrator = true;
    administrator.elevated = true;
    ServiceCore core(
        fixture.clock, registrations.value(), fixture.operations,
        fixture.worker, fixture.logger, fixture.ids, fixture.memory);
    const ServiceCommandRequest request{
        uuid("eeeeeee5-eeee-4eee-8eee-eeeeeeeeeee5"),
        control::DiskRepairProfileCommand{
            first_record.profile.profile_uuid, true},
        administrator,
        true,
        false,
        std::nullopt,
        ClientWaitState::active};
    auto queued = core.dispatch(request);
    REQUIRE(queued.ok());
    auto repaired = core.execute_operation(queued.value().operation_id.value());
    REQUIRE(repaired.ok());
    REQUIRE_EQ(repaired.value().state, OperationState::succeeded);
    auto loaded = registrations.value().load(
        first_record.profile.profile_uuid);
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value().record.revision, 3U);
    REQUIRE_EQ(loaded.value().record.profile, first_record.profile);
    REQUIRE_EQ(read_bytes(bundle.path()), sentinel);
}

TEST_CASE(native_forget_resumes_after_durable_removal_boundary) {
    TempBundle bundle;
    const std::vector<std::byte> sentinel{
        std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}};
    write_bytes(bundle.path(), sentinel);
    auto record = running_profile(product::PersistenceState::clean).record;
    record.profile.bundle_path = bundle.path().wstring();
    NativeStoreSandbox sandbox(record.profile.profile_uuid);
    auto profile_store = profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), record.profile.profile_uuid);
    REQUIRE(profile_store.ok());
    REQUIRE(profile_store.value().write_next(0, record, false).ok());
    AcceptingRepairVerifier verifier;
    auto registrations = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(registrations.ok());
    Fixture fixture(product::PersistenceState::clean);
    auto administrator = caller(record.profile.owner_sid);
    administrator.administrator = true;
    administrator.elevated = true;
    std::optional<product::Uuid> operation_id;
    {
        ServiceCore first(
            fixture.clock, registrations.value(), fixture.operations,
            fixture.worker, fixture.logger, fixture.ids, fixture.memory);
        const ServiceCommandRequest request{
            uuid("eeeeeee6-eeee-4eee-8eee-eeeeeeeeeee6"),
            control::DiskForgetCommand{
                record.profile.profile_uuid, record.profile.profile_uuid},
            administrator,
            true,
            false,
            std::nullopt,
            ClientWaitState::active};
        auto queued = first.dispatch(request);
        REQUIRE(queued.ok());
        operation_id = queued.value().operation_id;
    }
    REQUIRE(operation_id.has_value());
    auto tombstone = registrations.value().find_forget_tombstone(
        operation_id.value());
    REQUIRE(tombstone.ok());
    REQUIRE(tombstone.value().has_value());
    auto removal_started = tombstone.value().value();
    removal_started.phase = ForgetTombstonePhase::removal_started;
    auto encoded = encode_forget_tombstone(removal_started);
    REQUIRE(encoded.ok());
    constexpr std::array<std::wstring_view, 2> components{
        L"MyRamDisk", L"profiles"};
    auto root = profile::open_managed_atomic_directory(
        sandbox.program_data().wstring(), components);
    REQUIRE(root.ok());
    const auto operation_text = uuid_wide(operation_id.value());
    REQUIRE(profile::publish_atomically_with_previous(
        *root.value().directory,
        L"forget-" + operation_text + L".myft",
        L"forget-" + operation_text + L".prev",
        encoded.value()).ok());

    auto restarted_store = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(restarted_store.ok());
    ServiceCore restarted(
        fixture.clock, restarted_store.value(), fixture.operations,
        fixture.worker, fixture.logger, fixture.ids, fixture.memory);
    auto recovered = restarted.recover_after_restart();
    REQUIRE(recovered.ok());
    const auto operation = fixture.operations.load(operation_id.value());
    REQUIRE(operation.ok());
    REQUIRE_EQ(operation.value().state, OperationState::succeeded);
    auto completed_tombstone = restarted_store.value().find_forget_tombstone(
        operation_id.value());
    REQUIRE(completed_tombstone.ok());
    REQUIRE(completed_tombstone.value().has_value());
    REQUIRE_EQ(
        completed_tombstone.value()->phase,
        ForgetTombstonePhase::metadata_removed);
    REQUIRE(!restarted_store.value().load(record.profile.profile_uuid).ok());
    REQUIRE_EQ(read_bytes(bundle.path()), sentinel);

    const auto tombstone_path = sandbox.program_data() / L"MyRamDisk"
        / L"profiles" / (L"forget-" + operation_text + L".myft");
    auto corrupt_tombstone = read_bytes(tombstone_path);
    REQUIRE(!corrupt_tombstone.empty());
    corrupt_tombstone.back() ^= std::byte{0x80};
    write_bytes(tombstone_path, corrupt_tombstone);
    auto fallback_store = ProfileRegistrationStore::open_existing(
        sandbox.program_data().wstring(), verifier);
    REQUIRE(fallback_store.ok());
    auto recovered_from_previous = fallback_store.value().find_forget_tombstone(
        operation_id.value());
    REQUIRE(recovered_from_previous.ok());
    REQUIRE(recovered_from_previous.value().has_value());
    REQUIRE_EQ(
        recovered_from_previous.value()->phase,
        ForgetTombstonePhase::metadata_removed);
    auto preserved_tombstone = decode_forget_tombstone(read_bytes(
        sandbox.program_data() / L"MyRamDisk" / L"profiles"
        / (L"forget-" + operation_text + L".prev")));
    REQUIRE(preserved_tombstone.ok());
    REQUIRE_EQ(
        preserved_tombstone.value().phase,
        ForgetTombstonePhase::removal_started);
    REQUIRE_EQ(read_bytes(bundle.path()), sentinel);
}
