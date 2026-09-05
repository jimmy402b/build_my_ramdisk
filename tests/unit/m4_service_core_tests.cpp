#include "tests/support/test_framework.h"

#include "src/management/service_core.h"
#include "tests/support/m4_fake_clock.h"
#include "tests/support/m4_fake_profile_store.h"
#include "tests/support/m4_fake_worker.h"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

myrd::Error test_error(
    myrd::management::ServiceCoreError code,
    const wchar_t* operation) {
    return myrd::Error{
        myrd::ErrorDomain::internal,
        static_cast<std::uint32_t>(code),
        operation,
        L"Injected service-core test failure"};
}

myrd::Error operation_error(
    myrd::management::OperationStoreError code,
    const wchar_t* operation) {
    return myrd::Error{
        myrd::ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        operation,
        L"Injected operation-store test failure"};
}

myrd::management::ManagedProfileSnapshot ready_stopped_profile() {
    using namespace myrd::profile;
    Profile profile{
        uuid("00112233-4455-4677-8899-aabbccddeeff"),
        L"S-1-5-21-100-200-300-1001",
        L"work",
        1'700'000'000'000ULL,
        ProfileLifecycle::ready,
        L"D:\\Images\\work.myrd",
        "manifest.myrd",
        uuid("10213243-5465-4687-98a9-bacbdcedfe0f"),
        8ULL << 30,
        512,
        4096,
        uuid("20000000-0000-4000-8000-000000000001"),
        uuid("20000000-0000-4000-8000-000000000002"),
        uuid("20000000-0000-4000-8000-000000000003"),
        MountKind::drive_letter,
        L"R:",
        true,
        MemoryPolicy{
            MemoryMode::automatic,
            1ULL << 30,
            896ULL << 20,
            768ULL << 20,
            128ULL << 20},
        CheckpointPolicy{},
        CompactionPolicy{}};
    return myrd::management::ManagedProfileSnapshot{
        ProfileRecord{7, std::move(profile)},
        myrd::management::registered_profile_state(
            ProfileLifecycle::ready,
            myrd::product::DiskState{
                myrd::product::RuntimeState::stopped,
                myrd::product::ActivityState::idle,
                myrd::product::PersistenceState::clean,
                myrd::product::HealthState::healthy}),
        myrd::management::idle_operation_gate()};
}

myrd::ipc::CallerIdentity caller(std::wstring sid) {
    return myrd::ipc::CallerIdentity{
        std::move(sid), true, false, false, false, false, false, false};
}

class FakeOperationStore final
    : public myrd::management::IServiceOperationStore {
public:
    myrd::Result<std::optional<myrd::management::OperationAdmission>>
    find_request(
        const myrd::management::OperationRequest& request) override {
        ++find_calls;
        for (const auto& record : records) {
            if (record.request_id != request.request_id
                || record.caller_sid != request.caller_sid
                || record.command_scope != request.command_scope) {
                continue;
            }
            if (record.profile_id != request.profile_id
                || record.argument_digest != request.argument_digest) {
                return myrd::Result<std::optional<
                    myrd::management::OperationAdmission>>::failure(
                        operation_error(
                            myrd::management::OperationStoreError::
                                request_id_conflict,
                            L"fake_operation.find"));
            }
            return myrd::Result<std::optional<
                myrd::management::OperationAdmission>>::success(
                    myrd::management::OperationAdmission{record, true});
        }
        return myrd::Result<std::optional<
            myrd::management::OperationAdmission>>::success(std::nullopt);
    }

    myrd::Result<myrd::management::OperationAdmission> admit(
        const myrd::management::OperationRequest& request,
        const myrd::product::Uuid& candidate_operation_id) override {
        ++admit_calls;
        for (const auto& record : records) {
            if (record.request_id != request.request_id
                || record.caller_sid != request.caller_sid
                || record.command_scope != request.command_scope) {
                continue;
            }
            if (record.profile_id != request.profile_id
                || record.argument_digest != request.argument_digest) {
                return myrd::Result<
                    myrd::management::OperationAdmission>::failure(
                        operation_error(
                            myrd::management::OperationStoreError::
                                request_id_conflict,
                            L"fake_operation.admit"));
            }
            return myrd::Result<
                myrd::management::OperationAdmission>::success(
                    {record, true});
        }
        myrd::management::OperationRecord record{
            1,
            request.request_id,
            candidate_operation_id,
            request.profile_id,
            request.caller_sid,
            request.command_scope,
            request.argument_digest,
            myrd::management::OperationState::queued,
            "queued",
            request.created_unix_ms,
            request.created_unix_ms,
            std::nullopt,
            true,
            false,
            {}};
        records.push_back(record);
        history.push_back(record);
        return myrd::Result<
            myrd::management::OperationAdmission>::success(
                {std::move(record), false});
    }

    myrd::Result<myrd::management::OperationRecord> load(
        const myrd::product::Uuid& operation_id) override {
        ++load_calls;
        const auto found = std::find_if(
            records.begin(), records.end(), [&](const auto& record) {
                return record.operation_id == operation_id;
            });
        if (found != records.end()) {
            return myrd::Result<
                myrd::management::OperationRecord>::success(*found);
        }
        return myrd::Result<myrd::management::OperationRecord>::failure(
            operation_error(
                myrd::management::OperationStoreError::operation_not_found,
                L"fake_operation.load"));
    }

    myrd::Result<void> write(
        std::uint64_t expected_revision,
        const myrd::management::OperationRecord& proposed) override {
        ++write_calls;
        if (before_write && !inside_write_callback) {
            inside_write_callback = true;
            before_write(write_calls);
            inside_write_callback = false;
        }
        if (failed_write_call.has_value()
            && failed_write_call.value() == write_calls) {
            return myrd::Result<void>::failure(operation_error(
                myrd::management::OperationStoreError::io_failure,
                L"fake_operation.write"));
        }
        const auto found = std::find_if(
            records.begin(), records.end(), [&](const auto& record) {
                return record.operation_id == proposed.operation_id;
            });
        if (found == records.end()) {
            return myrd::Result<void>::failure(operation_error(
                myrd::management::OperationStoreError::operation_not_found,
                L"fake_operation.write"));
        }
        if (found->revision != expected_revision
            || proposed.revision != expected_revision + 1) {
            return myrd::Result<void>::failure(operation_error(
                myrd::management::OperationStoreError::revision_conflict,
                L"fake_operation.write"));
        }
        *found = proposed;
        history.push_back(proposed);
        return myrd::Result<void>::success();
    }

    myrd::Result<std::vector<myrd::management::OperationRecord>>
    reconcile_after_restart(std::uint64_t now_unix_ms) override {
        ++reconcile_calls;
        for (auto& record : records) {
            if (record.state != myrd::management::OperationState::running) {
                continue;
            }
            ++record.revision;
            record.state = myrd::management::OperationState::reconciling;
            record.phase = "restart-reconcile";
            record.updated_unix_ms = (std::max)(
                record.updated_unix_ms, now_unix_ms);
            history.push_back(record);
        }
        return myrd::Result<
            std::vector<myrd::management::OperationRecord>>::success(records);
    }

    myrd::Result<myrd::management::OperationRecord> request_cancel(
        const myrd::product::Uuid& operation_id,
        std::uint64_t now_unix_ms) override {
        ++cancel_calls;
        auto loaded = load(operation_id);
        if (!loaded.ok()) return loaded;
        auto proposed = loaded.value();
        if (!proposed.cancellable) {
            return myrd::Result<
                myrd::management::OperationRecord>::failure(operation_error(
                    myrd::management::OperationStoreError::
                        cancellation_too_late,
                    L"fake_operation.cancel"));
        }
        proposed.revision += 1;
        proposed.updated_unix_ms = now_unix_ms;
        proposed.cancel_requested = true;
        auto written = write(loaded.value().revision, proposed);
        if (!written.ok()) {
            return myrd::Result<
                myrd::management::OperationRecord>::failure(written.error());
        }
        return myrd::Result<myrd::management::OperationRecord>::success(
            std::move(proposed));
    }

    myrd::Result<myrd::management::OperationWaitObservation> wait_for_change(
        const myrd::product::Uuid& operation_id,
        std::uint64_t after_revision,
        std::uint64_t monotonic_deadline_ms) override {
        ++wait_calls;
        last_wait_after_revision = after_revision;
        last_wait_deadline_ms = monotonic_deadline_ms;
        auto loaded = load(operation_id);
        if (!loaded.ok()) {
            return myrd::Result<
                myrd::management::OperationWaitObservation>::failure(
                    loaded.error());
        }
        if (loaded.value().revision > after_revision) {
            return myrd::Result<
                myrd::management::OperationWaitObservation>::success(
                    {myrd::management::OperationWaitDisposition::changed,
                     loaded.move_value()});
        }
        return myrd::Result<
            myrd::management::OperationWaitObservation>::success(
                {myrd::management::OperationWaitDisposition::timed_out,
                 std::nullopt});
    }

    std::size_t admit_calls{};
    std::size_t find_calls{};
    std::size_t load_calls{};
    std::size_t write_calls{};
    std::size_t reconcile_calls{};
    std::size_t cancel_calls{};
    std::size_t wait_calls{};
    std::uint64_t last_wait_after_revision{};
    std::uint64_t last_wait_deadline_ms{};
    std::optional<std::size_t> failed_write_call;
    std::function<void(std::size_t)> before_write;
    bool inside_write_callback{};
    std::vector<myrd::management::OperationRecord> records;
    std::vector<myrd::management::OperationRecord> history;
};

class FakeLogger final : public myrd::management::IServiceLogger {
public:
    void write(
        const myrd::management::ServiceLogEvent& event) noexcept override {
        events.push_back(event);
    }
    std::vector<myrd::management::ServiceLogEvent> events;
};

class FakeIds final : public myrd::management::IServiceIdGenerator {
public:
    myrd::Result<myrd::product::Uuid> next() override {
        ++calls;
        if (values.empty()) {
            return myrd::Result<myrd::product::Uuid>::failure(test_error(
                myrd::management::ServiceCoreError::dependency_failure,
                L"fake_ids.next"));
        }
        auto value = values.front();
        values.erase(values.begin());
        return myrd::Result<myrd::product::Uuid>::success(std::move(value));
    }
    std::size_t calls{};
    std::vector<myrd::product::Uuid> values;
};

class FakeMemory final : public myrd::management::IPhysicalMemoryQuery {
public:
    myrd::Result<myrd::management::PhysicalMemorySnapshot> query() override {
        ++calls;
        return myrd::Result<
            myrd::management::PhysicalMemorySnapshot>::success(
                {16ULL << 30, 8ULL << 30});
    }
    std::size_t calls{};
};

} // namespace

static_assert(std::is_abstract_v<myrd::management::IServiceClock>);
static_assert(std::is_abstract_v<myrd::management::IServiceProfileStore>);
static_assert(std::is_abstract_v<myrd::management::IServiceOperationStore>);
static_assert(std::is_abstract_v<myrd::management::IWorkerControl>);
static_assert(std::is_abstract_v<myrd::management::IServiceLogger>);
static_assert(std::is_abstract_v<myrd::management::IServiceIdGenerator>);
static_assert(std::is_abstract_v<myrd::management::IPhysicalMemoryQuery>);

TEST_CASE(service_core_dependency_boundary_is_explicit_and_abstract) {
    REQUIRE(!std::is_copy_constructible_v<myrd::management::ServiceCore>);
    REQUIRE(!std::is_copy_assignable_v<myrd::management::ServiceCore>);
}

TEST_CASE(service_core_authorized_disk_show_is_a_side_effect_free_query) {
    const auto expected = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(expected);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    const auto request_id = uuid(
        "30000000-0000-4000-8000-000000000003");
    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        request_id,
        myrd::control::DiskShowCommand{L"work"},
        caller(expected.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(response.ok());
    REQUIRE_EQ(response.value().request_id, request_id);
    REQUIRE_EQ(response.value().command, "disk.show");
    REQUIRE_EQ(
        response.value().disposition,
        myrd::management::ServiceResponseDisposition::completed);
    REQUIRE(response.value().profile.has_value());
    REQUIRE_EQ(response.value().profile.value(), expected);
    REQUIRE(!response.value().operation_id.has_value());
    REQUIRE_EQ(operations.admit_calls, 0U);
    REQUIRE_EQ(operations.load_calls, 0U);
    REQUIRE_EQ(profiles.publications.size(), 0U);
    REQUIRE_EQ(worker.calls.size(), 0U);
    REQUIRE_EQ(ids.calls, 0U);
    REQUIRE_EQ(memory.calls, 0U);
    REQUIRE_EQ(logger.events.size(), 1U);
    REQUIRE_EQ(
        logger.events.front().kind,
        myrd::management::ServiceLogKind::request_authorized);
}

TEST_CASE(service_core_disk_show_rechecks_owner_authorization) {
    const auto expected = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(expected);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskShowCommand{L"work"},
        caller(L"S-1-5-21-999-999-999-1001"),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(!response.ok());
    REQUIRE_EQ(
        response.error().code,
        static_cast<std::uint32_t>(
            myrd::management::ServiceCoreError::authorization_denied));
    REQUIRE_EQ(operations.admit_calls, 0U);
    REQUIRE_EQ(profiles.publications.size(), 0U);
    REQUIRE_EQ(worker.calls.size(), 0U);
    REQUIRE_EQ(logger.events.size(), 0U);
}

TEST_CASE(service_core_disk_start_persists_and_executes_state_machine_plan) {
    const auto initial = ready_stopped_profile();
    const auto plan = myrd::management::plan_command(
        initial.state, myrd::management::ManagementCommand::start);
    REQUIRE(plan.accepted());
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    worker.set_result({
        myrd::management::CompletionOutcome::succeeded_clean,
        {std::byte{'o'}, std::byte{'k'}}});
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    const auto request_id = uuid(
        "30000000-0000-4000-8000-000000000003");
    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        request_id,
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(response.ok());
    REQUIRE_EQ(
        response.value().disposition,
        myrd::management::ServiceResponseDisposition::operation_queued);
    REQUIRE_EQ(response.value().operation_id.value(), operation_id);
    REQUIRE_EQ(
        response.value().operation.value().state,
        myrd::management::OperationState::running);
    REQUIRE_EQ(worker.calls.size(), 0U);
    auto terminal = core.execute_operation(operation_id);
    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::succeeded);
    REQUIRE_EQ(
        terminal.value().terminal_result,
        std::vector<std::byte>({std::byte{'o'}, std::byte{'k'}}));
    REQUIRE_EQ(operations.history.size(), 4U);
    REQUIRE_EQ(
        operations.history[0].state,
        myrd::management::OperationState::queued);
    REQUIRE_EQ(
        operations.history[2].state,
        myrd::management::OperationState::running);
    REQUIRE_EQ(
        operations.history[3].state,
        myrd::management::OperationState::succeeded);
    REQUIRE_EQ(worker.calls.size(), plan.action_span().size());
    for (std::size_t index = 0; index < worker.calls.size(); ++index) {
        REQUIRE_EQ(worker.calls[index].action, plan.action_span()[index]);
        REQUIRE_EQ(worker.calls[index].operation_id, operation_id);
        REQUIRE_EQ(
            worker.calls[index].profile_id,
            initial.record.profile.profile_uuid);
    }
    REQUIRE_EQ(profiles.publications.size(), 3U);
    REQUIRE_EQ(profiles.publications[0].state, plan.admitted);
    REQUIRE_EQ(
        profiles.publications[0].operation_gate.active_operation_id.value(),
        operation_id);
    const auto completed = myrd::management::complete_command(
        plan, myrd::management::CompletionOutcome::succeeded_clean);
    REQUIRE(completed.applied());
    REQUIRE_EQ(profiles.publications[1].state, completed.after);
    REQUIRE(profiles.publications[1]
                .operation_gate.active_operation_id.has_value());
    REQUIRE_EQ(profiles.publications[2].state, completed.after);
    REQUIRE(!profiles.publications[2]
                 .operation_gate.active_operation_id.has_value());

    auto replay = core.dispatch(myrd::management::ServiceCommandRequest{
        request_id,
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(replay.ok());
    REQUIRE_EQ(replay.value().operation.value(),
               terminal.value());
    REQUIRE(replay.value().warnings.empty());
    REQUIRE_EQ(worker.calls.size(), plan.action_span().size());
    REQUIRE_EQ(ids.calls, 1U);
}

TEST_CASE(service_core_no_wait_returns_after_durable_admission) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(response.ok());
    REQUIRE_EQ(
        response.value().disposition,
        myrd::management::ServiceResponseDisposition::operation_queued);
    REQUIRE_EQ(response.value().operation_id.value(), operation_id);
    REQUIRE_EQ(
        response.value().operation.value().state,
        myrd::management::OperationState::running);
    REQUIRE_EQ(operations.history.size(), 2U);
    REQUIRE_EQ(profiles.publications.size(), 1U);
    REQUIRE_EQ(worker.calls.size(), 0U);
}

TEST_CASE(service_core_client_wait_cancel_does_not_cancel_server_operation) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    worker.set_result({
        myrd::management::CompletionOutcome::succeeded_clean,
        {std::byte{'o'}, std::byte{'k'}}});
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::cancelled});

    REQUIRE(response.ok());
    REQUIRE_EQ(
        response.value().disposition,
        myrd::management::ServiceResponseDisposition::client_wait_cancelled);
    REQUIRE_EQ(worker.calls.size(), 0U);
    REQUIRE(!operations.records.front().cancel_requested);
    auto terminal = core.execute_operation(operation_id);
    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::succeeded);
}

TEST_CASE(service_core_zero_wait_timeout_does_not_stop_server_operation) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    worker.set_result({
        myrd::management::CompletionOutcome::succeeded_clean,
        {std::byte{'o'}, std::byte{'k'}}});
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        0,
        myrd::management::ClientWaitState::active});

    REQUIRE(response.ok());
    REQUIRE_EQ(
        response.value().disposition,
        myrd::management::ServiceResponseDisposition::wait_timeout);
    REQUIRE_EQ(worker.calls.size(), 0U);
    auto terminal = core.execute_operation(operation_id);
    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::succeeded);
}

TEST_CASE(service_core_exact_request_replay_returns_original_operation) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);
    const auto request_id = uuid(
        "30000000-0000-4000-8000-000000000003");
    const myrd::management::ServiceCommandRequest request{
        request_id,
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active};

    auto first = core.dispatch(request);
    auto replay = core.dispatch(request);

    REQUIRE(first.ok());
    REQUIRE(replay.ok());
    REQUIRE_EQ(replay.value().operation_id.value(), operation_id);
    REQUIRE_EQ(replay.value().operation.value(), first.value().operation.value());
    REQUIRE_EQ(ids.calls, 1U);
    REQUIRE_EQ(operations.admit_calls, 1U);
    REQUIRE_EQ(operations.find_calls, 2U);
    REQUIRE_EQ(profiles.publications.size(), 1U);
    REQUIRE_EQ(worker.calls.size(), 0U);
}

TEST_CASE(service_core_request_replay_rejects_changed_profile) {
    const auto first_profile = ready_stopped_profile();
    auto second_profile = ready_stopped_profile();
    second_profile.record.profile.profile_uuid = uuid(
        "00112233-4455-4677-8899-aabbccddeef0");
    second_profile.record.profile.display_name = L"other";
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(first_profile);
    profiles.add(second_profile);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    ids.values.push_back(uuid(
        "40000000-0000-4000-8000-000000000004"));
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);
    const auto request_id = uuid(
        "30000000-0000-4000-8000-000000000003");

    auto first = core.dispatch(myrd::management::ServiceCommandRequest{
        request_id,
        myrd::control::DiskStartCommand{L"work"},
        caller(first_profile.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    auto changed = core.dispatch(myrd::management::ServiceCommandRequest{
        request_id,
        myrd::control::DiskStartCommand{L"other"},
        caller(first_profile.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(first.ok());
    REQUIRE(!changed.ok());
    REQUIRE_EQ(
        changed.error().code,
        static_cast<std::uint32_t>(
            myrd::management::OperationStoreError::request_id_conflict));
    REQUIRE_EQ(operations.admit_calls, 1U);
    REQUIRE_EQ(profiles.publications.size(), 1U);
}

TEST_CASE(service_core_dry_run_preflights_without_mutation) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        true,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(response.ok());
    REQUIRE_EQ(
        response.value().disposition,
        myrd::management::ServiceResponseDisposition::dry_run);
    REQUIRE(response.value().dry_run_plan.has_value());
    REQUIRE(response.value().dry_run_plan.value().accepted());
    REQUIRE_EQ(operations.find_calls, 0U);
    REQUIRE_EQ(operations.admit_calls, 0U);
    REQUIRE_EQ(ids.calls, 0U);
    REQUIRE_EQ(profiles.publications.size(), 0U);
    REQUIRE_EQ(worker.calls.size(), 0U);
    REQUIRE_EQ(memory.calls, 1U);
}

TEST_CASE(service_core_operation_wait_is_bounded_and_client_cancel_is_local) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);
    auto queued = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(queued.ok());

    auto timed_out = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000013"),
        myrd::control::OperationWaitCommand{operation_id},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        45'000,
        myrd::management::ClientWaitState::active});
    REQUIRE(timed_out.ok());
    REQUIRE_EQ(
        timed_out.value().disposition,
        myrd::management::ServiceResponseDisposition::wait_timeout);
    REQUIRE_EQ(operations.wait_calls, 1U);
    REQUIRE_EQ(operations.last_wait_after_revision, 2U);
    REQUIRE_EQ(operations.last_wait_deadline_ms, 40'000ULL);
    REQUIRE_EQ(operations.cancel_calls, 0U);

    auto cancelled = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000023"),
        myrd::control::OperationWaitCommand{operation_id},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        1'000,
        myrd::management::ClientWaitState::cancelled});
    REQUIRE(cancelled.ok());
    REQUIRE_EQ(
        cancelled.value().disposition,
        myrd::management::ServiceResponseDisposition::client_wait_cancelled);
    REQUIRE_EQ(operations.wait_calls, 1U);
    REQUIRE_EQ(operations.cancel_calls, 0U);
    REQUIRE_EQ(
        operations.records.front().state,
        myrd::management::OperationState::running);
    REQUIRE(!operations.records.front().cancel_requested);
    REQUIRE_EQ(worker.calls.size(), 0U);
}

TEST_CASE(service_core_restart_reconciles_without_replaying_worker_actions) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);
    auto queued = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(queued.ok());
    operations.records.front().revision = 2;
    operations.records.front().state =
        myrd::management::OperationState::running;
    operations.records.front().phase = "running";

    auto recovered = core.recover_after_restart();

    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value().size(), 1U);
    REQUIRE_EQ(
        recovered.value().front().state,
        myrd::management::OperationState::reconciling);
    REQUIRE_EQ(operations.reconcile_calls, 1U);
    REQUIRE_EQ(worker.calls.size(), 0U);
    REQUIRE_EQ(logger.events.back().kind,
               myrd::management::ServiceLogKind::restart_reconcile);
}

TEST_CASE(service_core_failed_profile_admission_leaves_reconcilable_operation) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    profiles.fail_publication_at(1, test_error(
        myrd::management::ServiceCoreError::dependency_failure,
        L"fake_profile.publish"));
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    ids.values.push_back(uuid(
        "40000000-0000-4000-8000-000000000004"));
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(!response.ok());
    REQUIRE_EQ(operations.records.size(), 1U);
    REQUIRE_EQ(
        operations.records.front().state,
        myrd::management::OperationState::running);
    REQUIRE_EQ(operations.records.front().phase, "admitting");
    REQUIRE_EQ(profiles.publications.size(), 0U);
    REQUIRE_EQ(worker.calls.size(), 0U);
    profiles.clear_publication_failure();
    auto recovered = core.recover_after_restart();
    REQUIRE(recovered.ok());
    REQUIRE_EQ(
        recovered.value().front().state,
        myrd::management::OperationState::reconciling);
}

TEST_CASE(service_core_can_resume_queued_record_after_admitting_write_failure) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    operations.failed_write_call = 1U;
    myrd::tests::FakeWorkerControl worker;
    worker.set_result({
        myrd::management::CompletionOutcome::succeeded_clean,
        {std::byte{'o'}, std::byte{'k'}}});
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(!response.ok());
    REQUIRE_EQ(
        operations.records.front().state,
        myrd::management::OperationState::queued);
    REQUIRE_EQ(profiles.publications.size(), 0U);
    operations.failed_write_call.reset();

    auto terminal = core.execute_operation(operation_id);

    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::succeeded);
    REQUIRE_EQ(profiles.publications.size(), 3U);
}

TEST_CASE(service_core_terminal_write_failure_retains_exclusive_gate) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    operations.failed_write_call = 3U;
    myrd::tests::FakeWorkerControl worker;
    worker.set_result({
        myrd::management::CompletionOutcome::succeeded_clean,
        {std::byte{'o'}, std::byte{'k'}}});
    FakeLogger logger;
    FakeIds ids;
    ids.values.push_back(uuid(
        "40000000-0000-4000-8000-000000000004"));
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto admitted = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(admitted.ok());
    auto response = core.execute_operation(
        admitted.value().operation_id.value());
    REQUIRE(!response.ok());
    REQUIRE_EQ(
        operations.records.front().state,
        myrd::management::OperationState::running);
    REQUIRE_EQ(profiles.publications.size(), 2U);
    REQUIRE(profiles.publications.back()
                .operation_gate.active_operation_id.has_value());
}

TEST_CASE(service_core_restart_releases_gate_after_terminal_publication_split) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    profiles.fail_publication_at(3, test_error(
        myrd::management::ServiceCoreError::dependency_failure,
        L"fake_profile.release"));
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    worker.set_result({
        myrd::management::CompletionOutcome::succeeded_clean,
        {std::byte{'o'}, std::byte{'k'}}});
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto admitted = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(admitted.ok());
    auto response = core.execute_operation(operation_id);
    REQUIRE(!response.ok());
    REQUIRE_EQ(
        operations.records.front().state,
        myrd::management::OperationState::succeeded);
    auto split_profile = profiles.load(initial.record.profile.profile_uuid);
    REQUIRE(split_profile.ok());
    REQUIRE_EQ(
        split_profile.value().operation_gate.active_operation_id.value(),
        operation_id);
    profiles.clear_publication_failure();
    auto recovered = core.recover_after_restart();
    REQUIRE(recovered.ok());
    auto repaired = profiles.load(initial.record.profile.profile_uuid);
    REQUIRE(repaired.ok());
    REQUIRE(!repaired.value().operation_gate.active_operation_id.has_value());
    REQUIRE_EQ(worker.calls.size(),
               myrd::management::plan_command(
                   initial.state,
                   myrd::management::ManagementCommand::start)
                   .action_span().size());
}

TEST_CASE(service_core_persists_noncancellable_boundary_before_worker_call) {
    const auto initial = ready_stopped_profile();
    const auto plan = myrd::management::plan_command(
        initial.state, myrd::management::ManagementCommand::start);
    REQUIRE(!plan.action_span().empty());
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    worker.add_noncancellable_boundary(plan.action_span().front());
    bool cancellation_rejected = false;
    worker.set_after_execute([&](std::size_t count) {
        if (count != 1U) return;
        auto cancellation = operations.request_cancel(
            uuid("40000000-0000-4000-8000-000000000004"),
            1'700'000'000'101ULL);
        cancellation_rejected = !cancellation.ok()
            && cancellation.error().code == static_cast<std::uint32_t>(
                myrd::management::OperationStoreError::
                    cancellation_too_late);
    });
    worker.set_result({
        myrd::management::CompletionOutcome::succeeded_clean,
        {std::byte{'o'}, std::byte{'k'}}});
    FakeLogger logger;
    FakeIds ids;
    ids.values.push_back(uuid(
        "40000000-0000-4000-8000-000000000004"));
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(response.ok());
    auto terminal = core.execute_operation(
        response.value().operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(operations.history.size(), 5U);
    REQUIRE_EQ(
        operations.history[3].state,
        myrd::management::OperationState::running);
    REQUIRE(!operations.history[3].cancellable);
    REQUIRE_EQ(operations.history[3].phase, "noncancellable");
    REQUIRE_EQ(profiles.publications.size(), 4U);
    REQUIRE(!profiles.publications[1].operation_gate.cancellable);
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::succeeded);
    REQUIRE(!terminal.value().cancellable);
    REQUIRE(cancellation_rejected);
}

TEST_CASE(service_core_worker_failure_is_terminal_and_releases_profile_gate) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    worker.set_failure(test_error(
        myrd::management::ServiceCoreError::dependency_failure,
        L"fake_worker.execute"));
    FakeLogger logger;
    FakeIds ids;
    ids.values.push_back(uuid(
        "40000000-0000-4000-8000-000000000004"));
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(response.ok());
    auto terminal = core.execute_operation(
        response.value().operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(worker.calls.size(), 1U);
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::failed);
    REQUIRE(!terminal.value().terminal_result.empty());
    REQUIRE_EQ(profiles.publications.size(), 3U);
    REQUIRE_EQ(profiles.publications.back().state, initial.state);
    REQUIRE(!profiles.publications.back()
                 .operation_gate.active_operation_id.has_value());
}

TEST_CASE(service_core_honors_durable_cancel_before_first_worker_action) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);
    auto queued = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(queued.ok());
    auto cancelled = operations.request_cancel(
        operation_id, 1'700'000'000'101ULL);
    REQUIRE(cancelled.ok());

    auto terminal = core.execute_operation(operation_id);

    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::cancelled);
    REQUIRE(terminal.value().cancel_requested);
    REQUIRE(!terminal.value().cancellable);
    REQUIRE(!terminal.value().terminal_result.empty());
    REQUIRE_EQ(worker.calls.size(), 0U);
    REQUIRE_EQ(profiles.publications.back().state, initial.state);
    REQUIRE(!profiles.publications.back()
                 .operation_gate.active_operation_id.has_value());
}

TEST_CASE(service_core_honors_durable_cancel_between_worker_actions) {
    const auto initial = ready_stopped_profile();
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    worker.set_after_execute([&](std::size_t count) {
        if (count == 1U) {
            static_cast<void>(operations.request_cancel(
                operation_id, 1'700'000'000'101ULL));
        }
    });
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);
    auto queued = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(queued.ok());

    auto terminal = core.execute_operation(operation_id);

    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::cancelled);
    REQUIRE_EQ(worker.calls.size(), 1U);
    REQUIRE_EQ(profiles.publications.back().state, initial.state);
}

TEST_CASE(service_core_cancel_wins_race_with_noncancellable_transition) {
    const auto initial = ready_stopped_profile();
    const auto plan = myrd::management::plan_command(
        initial.state, myrd::management::ManagementCommand::start);
    REQUIRE(!plan.action_span().empty());
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    worker.add_noncancellable_boundary(plan.action_span().front());
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);
    auto queued = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(queued.ok());
    operations.before_write = [&](std::size_t call) {
        if (call == 3U) {
            static_cast<void>(operations.request_cancel(
                operation_id, 1'700'000'000'101ULL));
        }
    };

    auto terminal = core.execute_operation(operation_id);

    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::cancelled);
    REQUIRE(terminal.value().cancel_requested);
    REQUIRE_EQ(worker.calls.size(), 0U);
    REQUIRE_EQ(profiles.publications.back().state, initial.state);
    REQUIRE(!profiles.publications.back()
                 .operation_gate.active_operation_id.has_value());
}

TEST_CASE(service_core_honors_cancel_accepted_during_last_worker_action) {
    const auto initial = ready_stopped_profile();
    const auto plan = myrd::management::plan_command(
        initial.state, myrd::management::ManagementCommand::start);
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    const auto operation_id = uuid(
        "40000000-0000-4000-8000-000000000004");
    ids.values.push_back(operation_id);
    worker.set_after_execute([&](std::size_t count) {
        if (count == plan.action_span().size()) {
            static_cast<void>(operations.request_cancel(
                operation_id, 1'700'000'000'101ULL));
        }
    });
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);
    auto admitted = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(admitted.ok());

    auto terminal = core.execute_operation(operation_id);

    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::cancelled);
    REQUIRE_EQ(worker.calls.size(), plan.action_span().size());
    REQUIRE_EQ(profiles.publications.back().state, initial.state);
}

TEST_CASE(service_core_start_already_running_is_durable_noop_with_warning) {
    auto initial = ready_stopped_profile();
    initial.state.disk.runtime = myrd::product::RuntimeState::running;
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(initial);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    ids.values.push_back(uuid(
        "40000000-0000-4000-8000-000000000004"));
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto response = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});

    REQUIRE(response.ok());
    auto terminal = core.execute_operation(
        response.value().operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(
        terminal.value().state,
        myrd::management::OperationState::succeeded);
    REQUIRE_EQ(response.value().warnings.size(), 1U);
    REQUIRE_EQ(
        response.value().warnings.front(),
        myrd::control::IdempotentWarning::already_running);
    REQUIRE_EQ(operations.history.size(), 4U);
    REQUIRE_EQ(worker.calls.size(), 0U);
    REQUIRE_EQ(profiles.publications.size(), 0U);

    auto replay = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(initial.record.profile.owner_sid),
        false,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    REQUIRE(replay.ok());
    REQUIRE_EQ(replay.value().operation.value(),
               terminal.value());
    REQUIRE_EQ(replay.value().warnings.size(), 1U);
    REQUIRE_EQ(
        replay.value().warnings.front(),
        myrd::control::IdempotentWarning::already_running);
    REQUIRE_EQ(ids.calls, 1U);
    REQUIRE_EQ(worker.calls.size(), 0U);
}

TEST_CASE(service_core_serializes_one_profile_but_admits_another_profile) {
    const auto first_profile = ready_stopped_profile();
    auto second_profile = ready_stopped_profile();
    second_profile.record.profile.profile_uuid = uuid(
        "00112233-4455-4677-8899-aabbccddeef0");
    second_profile.record.profile.display_name = L"other";
    myrd::tests::FakeServiceClock clock(1'700'000'000'100ULL, 10'000);
    myrd::tests::FakeServiceProfileStore profiles;
    profiles.add(first_profile);
    profiles.add(second_profile);
    FakeOperationStore operations;
    myrd::tests::FakeWorkerControl worker;
    FakeLogger logger;
    FakeIds ids;
    const auto first_operation = uuid(
        "40000000-0000-4000-8000-000000000004");
    const auto second_operation = uuid(
        "40000000-0000-4000-8000-000000000014");
    ids.values = {first_operation, second_operation};
    FakeMemory memory;
    myrd::management::ServiceCore core(
        clock, profiles, operations, worker, logger, ids, memory);

    auto first = core.dispatch(myrd::management::ServiceCommandRequest{
        uuid("30000000-0000-4000-8000-000000000003"),
        myrd::control::DiskStartCommand{L"work"},
        caller(first_profile.record.profile.owner_sid),
        true,
        false,
        std::nullopt,
        myrd::management::ClientWaitState::active});
    auto same_profile = core.dispatch(
        myrd::management::ServiceCommandRequest{
            uuid("30000000-0000-4000-8000-000000000013"),
            myrd::control::DiskStartCommand{L"work"},
            caller(first_profile.record.profile.owner_sid),
            true,
            false,
            std::nullopt,
            myrd::management::ClientWaitState::active});
    auto other_profile = core.dispatch(
        myrd::management::ServiceCommandRequest{
            uuid("30000000-0000-4000-8000-000000000023"),
            myrd::control::DiskStartCommand{L"other"},
            caller(first_profile.record.profile.owner_sid),
            true,
            false,
            std::nullopt,
            myrd::management::ClientWaitState::active});

    REQUIRE(first.ok());
    REQUIRE(!same_profile.ok());
    REQUIRE_EQ(
        same_profile.error().code,
        static_cast<std::uint32_t>(
            myrd::management::ServiceCoreError::state_rejected));
    REQUIRE(other_profile.ok());
    REQUIRE_EQ(other_profile.value().operation_id.value(), second_operation);
    REQUIRE_EQ(operations.records.size(), 2U);
    REQUIRE_EQ(profiles.publications.size(), 2U);
    REQUIRE_EQ(worker.calls.size(), 0U);
}
