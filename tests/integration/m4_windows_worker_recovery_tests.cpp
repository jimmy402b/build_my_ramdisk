#include "src/management/windows_worker_recovery.h"
#include "src/management/windows_service.h"
#include "tests/support/test_framework.h"

#define NOMINMAX
#include <Windows.h>
#include <Sddl.h>

#include <chrono>
#include <filesystem>

namespace {

using namespace myrd;
using namespace myrd::management;
using namespace myrd::management::windows;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

WorkerRuntimeIdentity identity(const std::uint64_t session_generation = 7) {
    return {
        uuid("11111111-1111-4111-8111-111111111111"),
        uuid("22222222-2222-4222-8222-222222222222"),
        4242,
        9001,
        session_generation,
        5,
        product::kProductVersionMajor,
        product::kProductVersionMinor,
        product::kProductVersionPatch};
}

host::WorkerRuntimeStatus runtime_status(
    const std::uint64_t session_generation = 7) {
    return {
        {uuid("11111111-1111-4111-8111-111111111111"),
         uuid("22222222-2222-4222-8222-222222222222"),
         4242, 9001, session_generation, 5,
         product::kProductVersionMajor,
         product::kProductVersionMinor,
         product::kProductVersionPatch},
        {9, 8ULL << 30, 3, 0, 2, 0,
         uuid("44444444-4444-4444-8444-444444444444"),
         uuid("55555555-5555-4555-8555-555555555555"),
         uuid("66666666-6666-4666-8666-666666666666")},
        0x00020003U,
        11,
        L"\\BaseNamedObjects\\Global\\myrd-m1-0123456789abcdef0123456789abcdef",
        20,
        30,
        40,
        true,
        true,
        5,
        50'000,
        std::nullopt,
        0,
        false};
}

class NativeHistorySandbox final {
public:
    explicit NativeHistorySandbox(const product::Uuid& profile_id) {
        const auto token = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        root_ = std::filesystem::temp_directory_path()
            / (L"myrd-m4-worker-history-"
               + std::to_wstring(GetCurrentProcessId()) + L"-"
               + std::to_wstring(token));
        REQUIRE(std::filesystem::create_directories(program_data()));
        create_secure_directory(program_data() / L"MyRamDisk");
        create_secure_directory(program_data() / L"MyRamDisk" / L"profiles");
        const auto text = profile_id.to_string();
        create_secure_directory(
            profile_root(profile_id));
    }

    ~NativeHistorySandbox() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    std::filesystem::path program_data() const {
        return root_ / L"ProgramData";
    }

    std::filesystem::path profile_root(
        const product::Uuid& profile_id) const {
        const auto text = profile_id.to_string();
        return program_data() / L"MyRamDisk" / L"profiles"
            / std::wstring(text.begin(), text.end());
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

TEST_CASE(worker_recovery_operation_id_is_stable_and_identity_bound) {
    auto first = derive_worker_recovery_operation_id(identity());
    auto replay = derive_worker_recovery_operation_id(identity());
    auto successor = derive_worker_recovery_operation_id(identity(8));
    REQUIRE(first.ok());
    REQUIRE(replay.ok());
    REQUIRE(successor.ok());
    REQUIRE_EQ(first.value(), replay.value());
    REQUIRE(first.value() != successor.value());
}

TEST_CASE(worker_recovery_operation_id_rejects_invalid_identity) {
    auto invalid = identity();
    invalid.worker_pid = 0;
    REQUIRE(!derive_worker_recovery_operation_id(invalid).ok());
}

TEST_CASE(worker_restart_history_codec_is_exact_and_profile_bound) {
    const auto profile_id =
        uuid("11111111-1111-4111-8111-111111111111");
    WorkerRestartHistory history{
        profile_id,
        1,
        {{uuid("22222222-2222-4222-8222-222222222222"),
          uuid("33333333-3333-4333-8333-333333333333"),
          10'000,
          7,
          9,
          true}}};
    auto encoded = encode_worker_restart_history_for_test(history);
    REQUIRE(encoded.ok());
    auto decoded = decode_worker_restart_history_for_test(
        encoded.value(), profile_id);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), history);

    encoded.value().push_back(std::byte{});
    REQUIRE(!decode_worker_restart_history_for_test(
        encoded.value(), profile_id).ok());
    REQUIRE(!decode_worker_restart_history_for_test(
        std::span<const std::byte>(encoded.value()).first(
            encoded.value().size() - 1),
        uuid("44444444-4444-4444-8444-444444444444")).ok());
}

TEST_CASE(worker_restart_history_codec_rejects_unbounded_or_bad_attempts) {
    const auto profile_id =
        uuid("11111111-1111-4111-8111-111111111111");
    WorkerRestartHistory empty{profile_id, 1, {}};
    REQUIRE(!encode_worker_restart_history_for_test(empty).ok());
    WorkerRestartHistory invalid{
        profile_id,
        1,
        {{uuid("22222222-2222-4222-8222-222222222222"),
          uuid("33333333-3333-4333-8333-333333333333"),
          0,
          7,
          9,
          true}}};
    REQUIRE(!encode_worker_restart_history_for_test(invalid).ok());
}

TEST_CASE(worker_restart_history_clear_removes_both_generations_and_is_idempotent) {
    const auto profile_id =
        uuid("11111111-1111-4111-8111-111111111111");
    NativeHistorySandbox sandbox(profile_id);
    DurableWorkerRestartHistoryStore store(
        sandbox.program_data().wstring());
    const WorkerRestartAttempt first_attempt{
        uuid("22222222-2222-4222-8222-222222222222"),
        uuid("33333333-3333-4333-8333-333333333333"),
        10'000,
        7,
        9,
        true};
    WorkerRestartHistory first{profile_id, 1, {first_attempt}};
    REQUIRE(store.write_next(profile_id, std::nullopt, first).ok());
    WorkerRestartHistory second{profile_id, 2, {first_attempt}};
    second.attempts.front().attempted_at_unix_ms = 20'000;
    REQUIRE(store.write_next(profile_id, 1, second).ok());

    const auto root = sandbox.profile_root(profile_id);
    REQUIRE(std::filesystem::exists(root / L"worker.restart"));
    REQUIRE(std::filesystem::exists(root / L"worker.restart.prev"));

    REQUIRE(store.clear(profile_id).ok());
    REQUIRE(!std::filesystem::exists(root / L"worker.restart"));
    REQUIRE(!std::filesystem::exists(root / L"worker.restart.prev"));
    auto loaded = store.load(profile_id);
    REQUIRE(loaded.ok());
    REQUIRE(!loaded.value().has_value());
    REQUIRE(store.clear(profile_id).ok());
}

TEST_CASE(live_reconnect_accepts_only_the_same_worker_and_monotonic_runtime) {
    const auto previous = runtime_status();
    auto next = previous;
    ++next.binding.session_generation;
    ++next.heartbeat_sequence;
    ++next.snapshot_sequence;
    ++next.dirty_epoch;
    ++next.committed_generation;
    REQUIRE(live_reconnect_status_is_safe_for_test(previous, next));

    auto changed_device = next;
    ++changed_device.device.physical_drive_number;
    REQUIRE(!live_reconnect_status_is_safe_for_test(
        previous, changed_device));
    auto changed_process = next;
    ++changed_process.binding.worker_pid;
    REQUIRE(!live_reconnect_status_is_safe_for_test(
        previous, changed_process));
    auto stale_sequence = next;
    stale_sequence.snapshot_sequence = previous.snapshot_sequence;
    REQUIRE(!live_reconnect_status_is_safe_for_test(
        previous, stale_sequence));
    auto stale_generation = next;
    stale_generation.binding.session_generation =
        previous.binding.session_generation;
    REQUIRE(!live_reconnect_status_is_safe_for_test(
        previous, stale_generation));
}

TEST_CASE(live_reconnect_recovers_after_rotation_succeeded_but_status_was_lost) {
    const auto previous = runtime_status();
    auto managed = previous.binding;
    ++managed.session_generation;
    REQUIRE(live_reconnect_binding_is_safe_for_test(
        previous, managed, managed));

    auto visible_successor = managed;
    ++visible_successor.session_generation;
    REQUIRE(live_reconnect_binding_is_safe_for_test(
        previous, managed, visible_successor));

    auto skipped = visible_successor;
    ++skipped.session_generation;
    REQUIRE(!live_reconnect_binding_is_safe_for_test(
        previous, managed, skipped));
    auto changed_process = managed;
    ++changed_process.worker_pid;
    REQUIRE(!live_reconnect_binding_is_safe_for_test(
        previous, managed, changed_process));

    auto recovered = previous;
    recovered.binding = managed;
    ++recovered.snapshot_sequence;
    ++recovered.heartbeat_sequence;
    REQUIRE(managed_live_status_is_safe_for_test(
        previous, managed, recovered));
    auto changed_device = recovered;
    ++changed_device.device.physical_drive_number;
    REQUIRE(!managed_live_status_is_safe_for_test(
        previous, managed, changed_device));
}

TEST_CASE(scm_pending_status_progress_runs_independently_and_stops_cleanly) {
    auto pulses = scm_pending_status_pulse_count_for_test();
    REQUIRE(pulses.ok());
    REQUIRE(pulses.value() >= 2U);
}

TEST_CASE(scm_stop_pending_progress_covers_checkpoint_prework) {
    auto pulses = scm_stop_pending_covers_prework_for_test();
    REQUIRE(pulses.ok());
    REQUIRE(pulses.value() >= 2U);
}

TEST_CASE(scm_stop_request_claim_is_idempotent_and_reusable_after_fail_closed) {
    auto pulses = scm_stop_request_claim_lifecycle_for_test();
    REQUIRE(pulses.ok());
    REQUIRE(pulses.value() >= 4U);
}

} // namespace
