#include "tests/support/test_framework.h"

#include "src/management/service_operation_store.h"

#define NOMINMAX
#include <Windows.h>
#include <Sddl.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

std::array<std::byte, 32> argument_digest() {
    std::array<std::byte, 32> digest{};
    for (std::size_t index = 0; index != digest.size(); ++index) {
        digest[index] = static_cast<std::byte>(index + 1U);
    }
    return digest;
}

myrd::product::Uuid numbered_uuid(std::uint32_t value) {
    char text[37]{};
    const int written = std::snprintf(
        text, sizeof(text), "90000000-0000-4000-8000-%012x", value);
    REQUIRE_EQ(written, 36);
    return uuid(text);
}

myrd::management::OperationRequest checkpoint_request() {
    return myrd::management::OperationRequest{
        uuid("40000000-0000-4000-8000-000000000004"),
        uuid("30000000-0000-4000-8000-000000000003"),
        L"S-1-5-21-100-200-300-1001",
        "disk.checkpoint",
        argument_digest(),
        1700000000000ULL};
}

class TestDirectory final {
public:
    TestDirectory() {
        static std::atomic_uint32_t sequence{};
        root_ = std::filesystem::temp_directory_path()
            / (L"myrd-m5-service-operation-store-"
               + std::to_wstring(GetCurrentProcessId()) + L"-"
               + std::to_wstring(sequence.fetch_add(1)));
        // A run that died before its destructor ran leaves its tree behind, and
        // the next run can be handed the same recycled pid and sequence.  Clear
        // it rather than requiring the creation to have been the first.
        std::error_code stale;
        std::filesystem::remove_all(root_, stale);
        REQUIRE(std::filesystem::create_directories(program_data()));
    }
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    std::filesystem::path program_data() const {
        return root_ / L"ProgramData";
    }

private:
    std::filesystem::path root_;
};

void create_secure_directory(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
        &descriptor, nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const auto error = created ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    REQUIRE(created || error == ERROR_ALREADY_EXISTS);
}

void create_operation_layout(const std::filesystem::path& program_data) {
    create_secure_directory(program_data / L"MyRamDisk");
    create_secure_directory(program_data / L"MyRamDisk" / L"operations");
}

// The wait deadline is stated on the service clock, so the fake stands in for
// exactly one question: what time does the service think it is.
class SteppingClock final : public myrd::management::IServiceClock {
public:
    std::uint64_t unix_time_ms() const noexcept override {
        return 1700000000000ULL;
    }
    std::uint64_t monotonic_time_ms() const noexcept override {
        std::uint64_t observed = now_;
        // Fires at most once, on the first query of a wait: the moment the
        // waiting service checks the clock is the moment another writer
        // commits, which is what a real restart or pipe client would do.
        if (on_query_) {
            auto hook = std::move(on_query_);
            on_query_ = {};
            hook();
        }
        now_ += step_;
        return observed;
    }

    void set_step(std::uint64_t step) noexcept { step_ = step; }
    void on_next_query(std::function<void()> hook) const {
        on_query_ = std::move(hook);
    }

private:
    mutable std::uint64_t now_{1000};
    std::uint64_t step_{};
    mutable std::function<void()> on_query_;
};

// Counts the yields instead of sleeping, so the polling loop is exercised
// without wall-clock time passing.
class CountingPace final : public myrd::management::IOperationWaitPace {
public:
    void wait_before_repoll() noexcept override { ++repolls; }
    std::uint64_t repolls{};
};

} // namespace

TEST_CASE(service_operation_store_admits_a_caller_minted_operation_identifier) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto opened = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(opened.ok());
    SteppingClock clock;
    CountingPace pace;
    myrd::management::ServiceOperationStore adapter(
        opened.move_value(), clock, pace);

    const auto request = checkpoint_request();
    auto absent = adapter.find_request(request);
    REQUIRE(absent.ok());
    REQUIRE(!absent.value().has_value());

    const auto candidate = numbered_uuid(1);
    auto admitted = adapter.admit(request, candidate);
    REQUIRE(admitted.ok());
    REQUIRE(!admitted.value().replayed);
    REQUIRE_EQ(admitted.value().record.operation_id, candidate);
    REQUIRE(admitted.value().record.state
            == myrd::management::OperationState::queued);

    auto found = adapter.find_request(request);
    REQUIRE(found.ok());
    REQUIRE(found.value().has_value());
    REQUIRE_EQ(found.value()->record.operation_id, candidate);
    auto loaded = adapter.load(candidate);
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value(), admitted.value().record);

    // A replay keeps the original identifier even when the caller proposes a
    // different one, so a retried request can never fork into two operations.
    auto replay = adapter.admit(request, numbered_uuid(2));
    REQUIRE(replay.ok());
    REQUIRE(replay.value().replayed);
    REQUIRE_EQ(replay.value().record.operation_id, candidate);

    // A second adapter over the same protected root sees the same truth: the
    // projection owns no state of its own.
    auto reopened = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(reopened.ok());
    myrd::management::ServiceOperationStore second(
        reopened.move_value(), clock, pace);
    auto from_second = second.load(candidate);
    REQUIRE(from_second.ok());
    REQUIRE_EQ(from_second.value(), admitted.value().record);
    auto second_find = second.find_request(request);
    REQUIRE(second_find.ok());
    REQUIRE(second_find.value().has_value());
    REQUIRE_EQ(second_find.value()->record.operation_id, candidate);
}

TEST_CASE(service_operation_store_refuses_a_taken_operation_identifier) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto opened = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(opened.ok());
    SteppingClock clock;
    CountingPace pace;
    myrd::management::ServiceOperationStore adapter(
        opened.move_value(), clock, pace);

    const auto candidate = numbered_uuid(1);
    auto first = adapter.admit(checkpoint_request(), candidate);
    REQUIRE(first.ok());

    // Another request may not claim an identifier that is already durable
    // truth, and the refusal leaves no trace of a second record.
    auto other = checkpoint_request();
    other.request_id = uuid("50000000-0000-4000-8000-000000000005");
    other.command_scope = "disk.compact";
    auto collision = adapter.admit(other, candidate);
    REQUIRE(!collision.ok());
    REQUIRE_EQ(collision.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::operation_id_conflict));
    REQUIRE_EQ(adapter.load(candidate).value(), first.value().record);
    REQUIRE(!adapter.find_request(other).value().has_value());

    // A reused request identifier with different arguments is a conflict, not
    // an admission and not an absent request.
    auto changed = checkpoint_request();
    changed.argument_digest.front() ^= std::byte{0xff};
    auto conflict = adapter.admit(changed, numbered_uuid(2));
    REQUIRE(!conflict.ok());
    REQUIRE_EQ(conflict.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::request_id_conflict));
    auto conflict_find = adapter.find_request(changed);
    REQUIRE(!conflict_find.ok());
    REQUIRE_EQ(conflict_find.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::request_id_conflict));
}

TEST_CASE(service_operation_store_projects_write_cas_reconcile_and_cancel) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto opened = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(opened.ok());
    SteppingClock clock;
    CountingPace pace;
    myrd::management::ServiceOperationStore adapter(
        opened.move_value(), clock, pace);

    const auto candidate = numbered_uuid(1);
    auto admitted = adapter.admit(checkpoint_request(), candidate);
    REQUIRE(admitted.ok());

    auto running = admitted.value().record;
    running.revision = 2;
    running.state = myrd::management::OperationState::running;
    running.phase = "dispatch";
    running.updated_unix_ms += 5;
    REQUIRE(adapter.write(1, running).ok());
    REQUIRE_EQ(adapter.load(candidate).value(), running);

    auto stale = running;
    stale.revision = 3;
    stale.phase = "stale";
    auto rejected = adapter.write(1, stale);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::revision_conflict));
    REQUIRE_EQ(adapter.load(candidate).value(), running);

    // A service restart reconciles the interrupted operation through the same
    // M4 path, and the pre-boundary phase is preserved.
    auto reconciled = adapter.reconcile_after_restart(1700000100000ULL);
    REQUIRE(reconciled.ok());
    REQUIRE_EQ(reconciled.value().size(), std::size_t{1});
    REQUIRE(reconciled.value().front().state
            == myrd::management::OperationState::reconciling);
    auto after_restart = adapter.load(candidate);
    REQUIRE(after_restart.ok());
    REQUIRE(after_restart.value().state
            == myrd::management::OperationState::reconciling);
    REQUIRE_EQ(after_restart.value().phase, std::string{"dispatch"});

    auto cancelled = adapter.request_cancel(candidate, 1700000200000ULL);
    REQUIRE(cancelled.ok());
    REQUIRE(cancelled.value().cancel_requested);
    auto repeated = adapter.request_cancel(candidate, 1700000200001ULL);
    REQUIRE(repeated.ok());
    REQUIRE_EQ(repeated.value(), cancelled.value());
    REQUIRE_EQ(adapter.load(candidate).value(), cancelled.value());
}

TEST_CASE(service_operation_store_wait_reports_change_and_bounded_timeout) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto opened = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(opened.ok());
    SteppingClock clock;
    CountingPace pace;
    myrd::management::ServiceOperationStore adapter(
        opened.move_value(), clock, pace);

    const auto candidate = numbered_uuid(1);
    auto admitted = adapter.admit(checkpoint_request(), candidate);
    REQUIRE(admitted.ok());

    // A revision newer than the caller's is a change, and it is reported
    // without polling or consulting the clock at all.
    auto changed = adapter.wait_for_change(candidate, 0, 1001);
    REQUIRE(changed.ok());
    REQUIRE(changed.value().disposition
            == myrd::management::OperationWaitDisposition::changed);
    REQUIRE(changed.value().record.has_value());
    REQUIRE_EQ(changed.value().record->revision, std::uint64_t{1});
    REQUIRE_EQ(pace.repolls, std::uint64_t{0});

    // An expired deadline is a timeout, and it still reports the durable
    // record so the caller can see how far the operation actually got.
    auto expired = adapter.wait_for_change(candidate, 1, 1000);
    REQUIRE(expired.ok());
    REQUIRE(expired.value().disposition
            == myrd::management::OperationWaitDisposition::timed_out);
    REQUIRE(expired.value().record.has_value());
    REQUIRE_EQ(expired.value().record->revision, std::uint64_t{1});

    // A live wait re-reads the store: the change committed while the caller
    // was waiting is observed, and the wait paces itself between polls.
    clock.set_step(1);
    clock.on_next_query([&] {
        auto second = myrd::management::OperationStore::open_existing(
            sandbox.program_data().wstring());
        REQUIRE(second.ok());
        auto advanced = admitted.value().record;
        advanced.revision = 2;
        advanced.state = myrd::management::OperationState::running;
        advanced.phase = "dispatch";
        advanced.updated_unix_ms += 5;
        REQUIRE(second.value().write_record(1, advanced).ok());
    });
    auto observed = adapter.wait_for_change(candidate, 1, 30'000);
    REQUIRE(observed.ok());
    REQUIRE(observed.value().disposition
            == myrd::management::OperationWaitDisposition::changed);
    REQUIRE(observed.value().record.has_value());
    REQUIRE_EQ(observed.value().record->revision, std::uint64_t{2});
    REQUIRE_EQ(pace.repolls, std::uint64_t{1});

    // An operation that is not durable truth at all is a failure, never a
    // silent timeout: the caller must not read "no change" out of a missing
    // operation.
    auto missing = adapter.wait_for_change(numbered_uuid(9), 1, 30'000);
    REQUIRE(!missing.ok());
    REQUIRE_EQ(missing.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::operation_not_found));
}
